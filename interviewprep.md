# Interview Prep — High-Concurrency Ticketing Engine (C++ · MySQL · Crow)

> Complete reference for understanding, explaining, and defending every design decision in a technical interview.

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Tech Stack — What and Why](#2-tech-stack--what-and-why)
3. [Project Structure](#3-project-structure)
4. [Complete Workflow — How a Request Flows](#4-complete-workflow--how-a-request-flows)
5. [Code Deep-Dive — Every File Explained](#5-code-deep-dive--every-file-explained)
6. [Key Tech Terms & Their Application Here](#6-key-tech-terms--their-application-here)
7. [The Core Problem — Preventing Double Booking](#7-the-core-problem--preventing-double-booking)
8. [MySQL Internals You Must Know](#8-mysql-internals-you-must-know)
9. [C++ Concepts Used in This Project](#9-c-concepts-used-in-this-project)
10. [HTTP & REST Concepts](#10-http--rest-concepts)
11. [Concurrency Concepts](#11-concurrency-concepts)
12. [Interview Questions & Answers](#12-interview-questions--answers)

---

## 1. Project Overview

This is a **backend web server** written in C++ that lets users book seats for an event. The hard problem it solves is: **what happens when 100 users try to book the same seat at exactly the same time?**

The answer must be: exactly one succeeds, everyone else gets a clean "already booked" error. No seat booked twice. No data corruption.

**Design philosophy — keep it simple:**
- No Redis cache
- No complex ORM
- No microservices
- One binary + MySQL + Crow HTTP server

---

## 2. Tech Stack — What and Why

| Technology | Role | Why chosen |
|---|---|---|
| **C++17** | Application language | Fine-grained thread control, zero-cost abstractions, fast |
| **Crow** | HTTP framework | Lightweight, header-only, built-in JSON, minimal setup |
| **MySQL 8.0** | Database | ACID transactions, InnoDB row-level locking (`SELECT FOR UPDATE`), widely used in industry |
| **mysql-connector-cpp** | MySQL C++ driver | Official driver, PreparedStatements, JDBC-style API |
| **CMake** | Build system | Industry standard for C++ |
| **vcpkg** | Package manager | Pulls Crow and mysql-connector-cpp automatically |
| **Docker Compose** | MySQL container | Reproducible DB environment, no local MySQL install needed |
| **std::mutex + std::condition_variable** | Thread safety | Serializes access to the connection pool |

**Why not Redis?**
Redis is a cache. Caches introduce stale-read risk — you could read "available" from Redis while the seat was just booked in MySQL. MySQL InnoDB is the single source of truth. Correctness over raw speed.

**Why not PostgreSQL?**
MySQL has better out-of-the-box C++ driver support via vcpkg and `mysql-connector-cpp`. The SQL concepts (transactions, locks) are identical.

**Why C++ over Go/Node?**
C++ makes the concepts explicit: manual thread management, explicit mutexes, no garbage collector. Every decision is visible in the code.

---

## 3. Project Structure

```
backend_projet/
├── CMakeLists.txt          ← Build config: finds deps, compiles, links
├── vcpkg.json              ← Declares crow + mysql-connector-cpp as deps
├── docker-compose.yml      ← MySQL 8.0 container with auto schema init
├── scripts/
│   └── setup.sh           ← One-command: Docker up + schema + cmake build
├── sql/
│   └── schema.sql         ← MySQL DDL: events + seats tables + seed data
├── src/
│   ├── main.cpp           ← Entry point: env config, pool init, Crow start
│   ├── db.h               ← PooledConnection + Db (connection pool) declarations
│   ├── db.cpp             ← Pool implementation: acquire, release, reconnect
│   ├── routes.h           ← register_routes() declaration
│   └── routes.cpp         ← All 5 API endpoint implementations
└── static/
    └── index.html         ← Frontend: JS polls /seats every 3s, click to book
```

---

## 4. Complete Workflow — How a Request Flows

### 4.1 Server Startup (`main.cpp`)

```
Binary starts
  → Read env vars (DB_HOST, DB_PORT, DB_USER, DB_PASS, DB_NAME, POOL_SIZE, PORT, THREADS)
  → Db db(host, port, user, pass, name, pool_size)
      → get_mysql_driver_instance()
      → loop pool_size times: driver->connect(url, user, pass) → setSchema()
      → all connections pushed into std::queue<sql::Connection*>
  → crow::SimpleApp app
  → register_routes(app, db)     ← attach URL handlers
  → app.port(18080).concurrency(4).run()   ← start 4 worker threads, block
```

The server blocks on `run()`. 4 OS threads are ready to handle HTTP requests.

### 4.2 Browser Opens the Page

```
Browser → GET /
  → routes.cpp opens static/index.html from disk
  → returns 200 text/html
Browser renders seat grid (5 rows × 10 columns = 50 seats)
JS calls fetchSeats() immediately, then polls every 3 seconds
```

### 4.3 Fetching Seats (`GET /seats?event_id=1`)

```
Browser → GET /seats?event_id=1

routes.cpp:
  → db.acquire()                              ← get a connection from pool
      (if pool empty, thread blocks on cv_.wait())
  → conn->prepareStatement(
        "SELECT id, seat_label, is_booked,
         COALESCE(booked_by,'') AS booked_by
         FROM seats WHERE event_id = ? ORDER BY seat_label"
    )
  → stmt->setInt(1, event_id)                 ← bind parameter safely
  → stmt->executeQuery()                      ← run query
  → loop result->next():
      read id, seat_label, is_booked, booked_by
      append to JSON string
  → PooledConnection destructs → connection returned to pool → cv.notify_one()
  → return 200 + JSON array
```

Response example:
```json
[
  {"id":1,"seat_label":"A1","is_booked":false,"booked_by":""},
  {"id":2,"seat_label":"A2","is_booked":true,"booked_by":"alice"}
]
```

### 4.4 Booking a Seat (`POST /book`) — The Critical Path

```
Browser → POST /book
Body: {"event_id":1, "seat_id":7, "user":"bob"}

routes.cpp:
  1. crow::json::load(req.body)       ← parse JSON
  2. validate event_id, seat_id, user
  3. db.acquire()                     ← get connection from pool
  4. conn->setAutoCommit(false)       ← BEGIN transaction
  5. PreparedStatement: SELECT id, is_booked, seat_label
                        FROM seats WHERE id=? AND event_id=?
                        FOR UPDATE
     → InnoDB places exclusive row lock on this seat row
     → any other transaction trying to lock same row BLOCKS here
  6. result->next() == false → rollback → 404
  7. is_booked == true → rollback → 409 Conflict
  8. PreparedStatement: UPDATE seats SET is_booked=1, booked_by=?, booked_at=NOW()
                        WHERE id=? AND event_id=?
  9. conn->commit()                   ← releases row lock
 10. conn->setAutoCommit(true)
 11. PooledConnection destructs → connection returned to pool
 12. return 200 {"success":true, ...}
```

### 4.5 Reset (`POST /reset`)

```
→ db.acquire()
→ conn->createStatement()->executeUpdate(
      "UPDATE seats SET is_booked=0, booked_by=NULL, booked_at=NULL"
  )
→ 200 OK
```

### 4.6 Health Check (`GET /health`)

```
→ db.available() > 0 → "healthy" or "degraded"
→ return {"status":"healthy","pool_available":10,"pool_total":10}
```

---

## 5. Code Deep-Dive — Every File Explained

### 5.1 `src/db.h` — Pool and RAII Wrapper

```cpp
// RAII wrapper — returns connection to pool on destruction
class PooledConnection {
public:
    PooledConnection(sql::Connection* conn,
                     std::function<void(sql::Connection*)> releaser);
    ~PooledConnection();

    PooledConnection(const PooledConnection&) = delete;      // non-copyable
    PooledConnection& operator=(const PooledConnection&) = delete;
    PooledConnection(PooledConnection&&) noexcept;           // movable
    PooledConnection& operator=(PooledConnection&&) noexcept;

    sql::Connection* operator->() { return conn_; }
    sql::Connection* get()        { return conn_; }

private:
    sql::Connection* conn_;
    std::function<void(sql::Connection*)> releaser_;  // lambda that calls pool.release()
};
```

**Why non-copyable?**
If you could copy a `PooledConnection`, two objects would own the same `sql::Connection*`. When the first destructs, it returns the connection to the pool. The second then also tries to return the same pointer — double-return bug. `= delete` makes this a compile error.

**Why movable?**
`db.acquire()` returns a `PooledConnection` by value. Move semantics transfer ownership from the temporary to the caller's variable — no copy, no double-return. After move, the source `conn_` is set to `nullptr` so its destructor does nothing.

```cpp
class Db {
public:
    Db(const std::string& host, int port,
       const std::string& user, const std::string& password,
       const std::string& database, size_t pool_size = 10);
    ~Db();

    PooledConnection acquire();    // blocks if pool empty
    size_t available() const;
    size_t pool_size() const;

private:
    void release(sql::Connection* conn);
    sql::Connection* create_connection();

    sql::mysql::MySQL_Driver* driver_;   // singleton — do NOT delete
    std::string url_, user_, password_, database_;
    size_t pool_size_;
    std::queue<sql::Connection*> pool_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};
```

---

### 5.2 `src/db.cpp` — Pool Implementation

**Constructor:**
```cpp
Db::Db(const std::string& host, int port, ...) {
    url_    = "tcp://" + host + ":" + std::to_string(port);
    driver_ = sql::mysql::get_mysql_driver_instance();  // MySQL singleton driver

    for (size_t i = 0; i < pool_size_; ++i) {
        pool_.push(create_connection());   // pre-create all connections at startup
    }
}

sql::Connection* Db::create_connection() {
    sql::Connection* conn = driver_->connect(url_, user_, password_);
    conn->setSchema(database_);           // USE ticketing;
    return conn;
}
```

`get_mysql_driver_instance()` returns the MySQL driver singleton — a global object that manages the driver lifecycle. You must not delete it.

**acquire() — the pool checkout:**
```cpp
PooledConnection Db::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !pool_.empty(); });  // block if empty

    sql::Connection* conn = pool_.front();
    pool_.pop();

    // Health check — reconnect if MySQL dropped the connection
    if (conn->isClosed()) {
        delete conn;
        conn = create_connection();
    }

    return PooledConnection(conn,
        [this](sql::Connection* c) { this->release(c); });
    // ↑ lambda: when PooledConnection destructs, calls release(c)
}
```

`cv_.wait(lock, predicate)` — atomically releases `mutex_` and suspends the thread. When `release()` calls `cv_.notify_one()`, the thread re-acquires `mutex_` and checks the predicate. Zero CPU usage while waiting.

**release() — return to pool:**
```cpp
void Db::release(sql::Connection* conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    pool_.push(conn);
    cv_.notify_one();   // wake one waiting thread
}
```

---

### 5.3 `src/main.cpp` — Entry Point

```cpp
auto get_env = [](const char* name, const char* default_val) -> std::string {
    const char* val = std::getenv(name);
    return val ? std::string(val) : std::string(default_val);
};
```

Lambda that reads an environment variable or returns a default. Lets you configure without recompiling:
```bash
DB_HOST=prod-db.internal POOL_SIZE=20 ./ticketing_engine
```

```cpp
Db db(db_host, db_port, db_user, db_pass, db_name, pool_sz);
```

This opens `pool_sz` MySQL connections at startup. If MySQL is down, the constructor throws `sql::SQLException` and the program exits with an error — fail fast, don't serve requests on a broken DB.

```cpp
crow::SimpleApp app;
register_routes(app, db);
app.port(port).concurrency(threads).run();
```

`concurrency(4)` creates 4 OS threads. All 4 share the same `Db` object. Thread safety of the pool (`std::mutex` + `std::condition_variable`) ensures concurrent `acquire()`/`release()` calls are safe.

---

### 5.4 `src/routes.cpp` — API Handlers

**Route registration:**
```cpp
CROW_ROUTE(app, "/seats")
([&db](const crow::request& req) {
    // ...
    return json_response(200, oss.str());
});
```

`CROW_ROUTE` is a macro that registers a URL pattern. `[&db]` captures `db` by reference — the lambda holds a reference to the actual `Db` object in `main`. Crow stores these lambdas and calls them when matching requests arrive on any worker thread.

**GET /seats — PreparedStatement:**
```cpp
auto conn = db.acquire();

std::unique_ptr<sql::PreparedStatement> stmt(
    conn->prepareStatement(
        "SELECT id, seat_label, is_booked, "
        "COALESCE(booked_by, '') AS booked_by "
        "FROM seats WHERE event_id = ? ORDER BY seat_label"
    )
);
stmt->setInt(1, event_id);
std::unique_ptr<sql::ResultSet> result(stmt->executeQuery());

while (result->next()) {
    int id             = result->getInt("id");
    std::string label  = result->getString("seat_label");
    bool is_booked     = result->getBoolean("is_booked");
    std::string by     = result->getString("booked_by");
    // build JSON...
}
```

`std::unique_ptr` owns the `PreparedStatement` and `ResultSet` — they are automatically deleted when they go out of scope. No manual `delete`. This is RAII applied to MySQL objects.

**POST /book — Transaction with SELECT FOR UPDATE:**
```cpp
auto conn = db.acquire();
conn->setAutoCommit(false);   // BEGIN transaction

try {
    // Acquires InnoDB exclusive row lock
    std::unique_ptr<sql::PreparedStatement> sel(
        conn->prepareStatement(
            "SELECT id, is_booked, seat_label "
            "FROM seats WHERE id = ? AND event_id = ? FOR UPDATE"
        )
    );
    sel->setInt(1, seat_id);
    sel->setInt(2, event_id);
    auto result = std::unique_ptr<sql::ResultSet>(sel->executeQuery());

    if (!result->next())       { conn->rollback(); return 404; }
    if (result->getBoolean("is_booked")) { conn->rollback(); return 409; }

    // UPDATE the seat
    std::unique_ptr<sql::PreparedStatement> upd(
        conn->prepareStatement(
            "UPDATE seats SET is_booked=1, booked_by=?, booked_at=NOW() "
            "WHERE id=? AND event_id=?"
        )
    );
    upd->setString(1, user);
    upd->setInt(2, seat_id);
    upd->setInt(3, event_id);
    upd->executeUpdate();

    conn->commit();              // releases row lock
    conn->setAutoCommit(true);
    return 200;

} catch (...) {
    conn->rollback();            // always rollback on error
    conn->setAutoCommit(true);
    throw;
}
```

The outer `catch (const sql::SQLException& e)` handles MySQL errors and returns 500.

---

### 5.5 `sql/schema.sql` — MySQL DDL

```sql
CREATE TABLE IF NOT EXISTS seats (
    id          INT AUTO_INCREMENT PRIMARY KEY,
    event_id    INT NOT NULL,
    seat_label  VARCHAR(10) NOT NULL,
    is_booked   TINYINT(1) NOT NULL DEFAULT 0,
    booked_by   VARCHAR(255),
    booked_at   DATETIME,
    UNIQUE KEY unique_seat (event_id, seat_label),
    INDEX idx_event (event_id),
    FOREIGN KEY (event_id) REFERENCES events(id) ON DELETE CASCADE
) ENGINE=InnoDB;
```

- `ENGINE=InnoDB` — required for transactions and `SELECT FOR UPDATE`
- `TINYINT(1)` — MySQL's boolean (0 = false, 1 = true)
- `UNIQUE KEY unique_seat` — composite unique constraint: same seat label allowed in different events, not within same event
- `INDEX idx_event` — speeds up `WHERE event_id = ?` queries
- `ON DELETE CASCADE` — deleting an event auto-deletes its seats

**Seed with Recursive CTE:**
```sql
INSERT IGNORE INTO seats (event_id, seat_label)
WITH RECURSIVE nums AS (
    SELECT 0 AS n
    UNION ALL
    SELECT n + 1 FROM nums WHERE n < 49
)
SELECT 1,
       CONCAT(CHAR(65 + (n DIV 10) USING utf8mb4),
              CAST((n MOD 10) + 1 AS CHAR))
FROM nums;
```

Generates 50 rows: n=0→"A1", n=1→"A2", ..., n=49→"E10". `CHAR(65)` = 'A', `CHAR(66)` = 'B', etc. `INSERT IGNORE` silently skips rows that violate the `UNIQUE KEY` — safe to run multiple times.

---

## 6. Key Tech Terms & Their Application Here

### Connection Pool
Pre-creating a set of database connections at startup and reusing them across requests. Creating a TCP connection + MySQL auth handshake takes ~5–10ms. Reusing a pooled connection takes ~microseconds.

In this project:
```
Db constructor → opens N connections → stores in std::queue<sql::Connection*>
acquire()      → pops one connection → gives to handler
release()      → pushes connection back → wakes waiting thread
```

### RAII (Resource Acquisition Is Initialization)
Tie resource lifetime to object lifetime. Constructor acquires, destructor releases. Used in three places:

1. `Db` destructor: calls `delete` on every `sql::Connection*` — MySQL connections closed cleanly
2. `PooledConnection` destructor: calls the releaser lambda → `Db::release()` → connection back to pool
3. `std::unique_ptr<sql::PreparedStatement>`: auto-deletes the statement object when it goes out of scope

### `std::unique_ptr`
Smart pointer with exclusive ownership. When it goes out of scope, it calls `delete` on the raw pointer. Used for all MySQL objects:
```cpp
std::unique_ptr<sql::PreparedStatement> stmt(conn->prepareStatement(...));
std::unique_ptr<sql::ResultSet> result(stmt->executeQuery());
// No manual delete needed — destructors handle it
```

### Prepared Statement
SQL pre-compiled by MySQL before parameters are bound. Two benefits:

1. **Security** — parameters never interpreted as SQL text → SQL injection impossible
2. **Performance** — query plan compiled once

```cpp
stmt->setInt(1, event_id);    // binds integer safely
stmt->setString(1, user);     // binds string safely
```
If a user sends `user = "'; DROP TABLE seats; --"`, it's stored as a string value, not executed as SQL.

### Transaction
Group of SQL operations that execute atomically (all-or-nothing). Started with `setAutoCommit(false)`, ended with `commit()` or `rollback()`.

```cpp
conn->setAutoCommit(false);  // BEGIN
// ... SELECT FOR UPDATE + UPDATE ...
conn->commit();              // or rollback() on failure
conn->setAutoCommit(true);   // restore default
```

### SELECT FOR UPDATE
MySQL InnoDB statement that reads a row AND places an exclusive lock on it. Any other transaction that tries to `SELECT FOR UPDATE` or `UPDATE` the same row will **block** until the first transaction commits or rolls back.

This is **pessimistic locking** — assume a conflict will happen, lock preemptively.

### ACID
- **Atomicity** — `setAutoCommit(false)` + `commit()`/`rollback()` — all-or-nothing
- **Consistency** — UNIQUE KEY prevents duplicate seat bookings at DB level
- **Isolation** — `SELECT FOR UPDATE` prevents concurrent transactions seeing each other's partial writes
- **Durability** — InnoDB redo log writes to disk before acknowledging commit; survives crashes

### InnoDB
MySQL's default storage engine. Provides: ACID transactions, row-level locking, MVCC, foreign keys, redo/undo logs. Required for `SELECT FOR UPDATE`. Alternative (MyISAM) only supports table-level locks — not suitable for concurrency.

### MVCC (Multi-Version Concurrency Control)
InnoDB keeps multiple versions of rows so plain `SELECT` queries can read a consistent snapshot without blocking writers. Only `SELECT FOR UPDATE` explicitly locks rows. This means `GET /seats` never blocks `POST /book`.

### std::mutex
A lock that only one thread can hold at a time. Used in `Db` to protect the `std::queue<sql::Connection*>`:
```cpp
std::lock_guard<std::mutex> lock(mutex_);   // acquire on construction
pool_.push(conn);                           // safe — only one thread here
cv_.notify_one();
// lock released at end of scope (RAII)
```

### std::condition_variable
Lets a thread sleep (zero CPU) until a condition is true. Used in `acquire()`:
```cpp
cv_.wait(lock, [this] { return !pool_.empty(); });
// Thread sleeps here if pool is empty
// Woken up by cv_.notify_one() in release()
```

Without this, waiting threads would spin in a loop burning CPU.

### Lambda
Anonymous inline function. Used for:

1. The releaser in `PooledConnection`:
```cpp
return PooledConnection(conn,
    [this](sql::Connection* c) { this->release(c); });
// When PooledConnection destructs, this lambda is called
```

2. Route handlers:
```cpp
CROW_ROUTE(app, "/health")
([&db]() {    // captures db by reference
    return ...;
});
```

3. The `cv_.wait` predicate:
```cpp
cv_.wait(lock, [this] { return !pool_.empty(); });
```

### HTTP Status Codes
| Code | Meaning | When returned |
|---|---|---|
| 200 | OK | Booking succeeded, seats fetched |
| 204 | No Content | CORS preflight response |
| 400 | Bad Request | Missing/invalid JSON fields |
| 404 | Not Found | Seat ID doesn't exist |
| 409 | Conflict | Seat already booked |
| 500 | Internal Server Error | MySQL exception |
| 503 | Service Unavailable | Health check: pool empty |

### CORS
Browser security policy: a page from `localhost:3000` cannot call APIs on `localhost:18080` unless the API allows it. We send:
```cpp
res.set_header("Access-Control-Allow-Origin", "*");
```
The `OPTIONS` handler answers the browser's preflight check before the real `POST`.

---

## 7. The Core Problem — Preventing Double Booking

### The Race Condition (without locks)

```
t=0ms: Thread A reads seat 7 → is_booked = 0  ✓
t=0ms: Thread B reads seat 7 → is_booked = 0  ✓  (both read before either writes)
t=1ms: Thread A: UPDATE seat 7 SET is_booked=1  → committed
t=1ms: Thread B: UPDATE seat 7 SET is_booked=1  → also committed  ← DOUBLE BOOKING
```

The problem: read-check-then-write is not atomic at the application level.

### Our Solution — InnoDB Row-Level Locking

```
Thread A: setAutoCommit(false)
Thread A: SELECT ... FROM seats WHERE id=7 FOR UPDATE
          → InnoDB locks row 7 exclusively

Thread B: SELECT ... FROM seats WHERE id=7 FOR UPDATE
          → Thread B BLOCKS — waits for row 7 lock

Thread A: UPDATE seats SET is_booked=1 WHERE id=7
Thread A: commit()   → row 7 lock released

Thread B: unblocks, reads row 7 → is_booked=1
Thread B: rollback() → returns 409 Conflict
```

Result: exactly one booking. Guaranteed by the database engine.

### Why Not Application-Level Mutex Instead?

You could use a `std::mutex` per seat. But:
- Doesn't work across multiple server processes (horizontal scaling)
- In-memory mutex is lost on crash
- `SELECT FOR UPDATE` works even with multiple server instances pointing at the same MySQL

Database-level locking is the correct layer for this problem.

---

## 8. MySQL Internals You Must Know

### InnoDB Locking Types
| Lock | SQL | Effect |
|---|---|---|
| Shared (S) | `SELECT ... LOCK IN SHARE MODE` | Multiple transactions can read; none can write |
| Exclusive (X) | `SELECT ... FOR UPDATE` | Only this transaction can read or write the row |

### Transaction Isolation Levels
MySQL InnoDB defaults to **REPEATABLE READ**. With `SELECT FOR UPDATE`, the locked rows behave at **SERIALIZABLE** level — no other transaction can modify them.

### setAutoCommit(false)
Disables MySQL's default behavior of committing each statement immediately. Starts an explicit transaction. All statements until `commit()` or `rollback()` are part of the same atomic unit.

### Deadlock
Circular wait: Transaction A holds lock on row 1, wants row 2. Transaction B holds lock on row 2, wants row 1. InnoDB detects this via wait-for graph and automatically rolls back the "victim" transaction (returns error 1213). Application should catch this and retry.

### Stale Connection (`isClosed()`)
MySQL server closes idle connections after `wait_timeout` (default 8 hours). The pool checks `conn->isClosed()` on acquire and reconnects transparently.

### INSERT IGNORE vs INSERT OR REPLACE
- `INSERT IGNORE` — skip rows that violate constraints silently (used for seed data)
- `INSERT OR REPLACE` — delete conflicting row, insert new one (changes primary key)

### Recursive CTE (Common Table Expression)
```sql
WITH RECURSIVE nums AS (
    SELECT 0 AS n
    UNION ALL
    SELECT n + 1 FROM nums WHERE n < 49
)
SELECT ... FROM nums;
```
Generates a series 0–49 in SQL. Used to seed 50 seats in one statement. Requires MySQL 8.0+.

### COALESCE
```sql
COALESCE(booked_by, '') AS booked_by
```
Returns first non-NULL argument. Since unbooked seats have `booked_by = NULL`, this returns `''` — easier to handle in JSON than `null`.

---

## 9. C++ Concepts Used in This Project

### Move Semantics
Moving transfers ownership of a resource without copying. `PooledConnection` is move-only:
```cpp
PooledConnection(PooledConnection&& other) noexcept
    : conn_(other.conn_), releaser_(std::move(other.releaser_)) {
    other.conn_     = nullptr;   // source no longer owns it
    other.releaser_ = nullptr;   // source destructor does nothing
}
```
`db.acquire()` returns `PooledConnection` by value — the move constructor kicks in, transferring `conn_` to the caller. No copy, no double-release.

### `= delete`
Explicitly prevents the compiler from generating a function:
```cpp
PooledConnection(const PooledConnection&) = delete;
```
Without this, C++ auto-generates a copy constructor that does a shallow copy of `conn_` — two objects pointing to the same MySQL connection. When first destructs, it returns the connection to pool. Second then tries to return same connection — corrupting the pool queue. `= delete` turns this bug into a compile error.

### `std::function`
Type-erased callable — can hold lambdas, function pointers, functors:
```cpp
std::function<void(sql::Connection*)> releaser_;
```
Stores the lambda `[this](sql::Connection* c) { this->release(c); }`. When `PooledConnection` destructs, it calls `releaser_(conn_)` — which invokes the lambda — which calls `Db::release()`. This decouples `PooledConnection` from `Db`.

### `std::unique_lock` vs `std::lock_guard`
- `lock_guard` — simple RAII lock, cannot be released early, cannot be moved
- `unique_lock` — RAII lock that can be released/re-acquired, required for `condition_variable::wait()`

```cpp
// acquire() needs unique_lock for cv_.wait()
std::unique_lock<std::mutex> lock(mutex_);
cv_.wait(lock, [this] { return !pool_.empty(); });
// cv_.wait() releases mutex while sleeping, re-acquires on wake
```

### `noexcept`
Marks a function as guaranteed not to throw exceptions. Move constructors should be `noexcept` — standard library containers (like `std::queue`) will use move instead of copy only if the move constructor is `noexcept`.

### `mutable`
Allows a `const` member function to modify a member variable. Used on `mutex_`:
```cpp
mutable std::mutex mutex_;
```
`available() const` needs to lock `mutex_` to safely read `pool_.size()`. Locking mutates the mutex but doesn't change the logical state of `Db`. `mutable` is the correct fix.

### Raw String Literal `R"(...)"`
```cpp
conn->prepareStatement(R"(
    SELECT id, seat_label
    FROM seats
    WHERE event_id = ?
)");
```
Everything between `R"(` and `)"` is literal — no escaping needed. Makes multiline SQL readable.

### `std::ostringstream`
In-memory string builder:
```cpp
std::ostringstream oss;
oss << "[";
while (result->next()) { oss << "{...}"; }
oss << "]";
return oss.str();
```
Avoids O(N²) string concatenation. Each `str1 + str2` allocates a new string. `ostringstream` accumulates into one buffer.

---

## 10. HTTP & REST Concepts

### REST
Architectural style using HTTP verbs + URLs to represent resources:

| Resource | Verb | URL | Action |
|---|---|---|---|
| Seat list | GET | /seats?event_id=1 | Read all seats |
| Booking | POST | /book | Create a booking |
| Reset | POST | /reset | Reset all seats |
| Health | GET | /health | Check server health |

### Request Anatomy
```
POST /book HTTP/1.1
Host: localhost:18080
Content-Type: application/json

{"event_id":1,"seat_id":7,"user":"alice"}
```

### How Crow Parses JSON
```cpp
auto body = crow::json::load(req.body);
if (!body) return json_response(400, R"({"error":"Invalid JSON"})");

int event_id = static_cast<int>(body["event_id"].i());   // integer
std::string user = body["user"].s();                      // string
```
`crow::json::load` returns a falsy value on invalid JSON. `.i()` = integer, `.s()` = string. Wrapped in `try/catch(...)` to handle missing keys.

### Query Parameters vs Body
```cpp
// Query param: /seats?event_id=1
auto event_id_str = req.url_params.get("event_id");

// Request body: {"event_id":1,"seat_id":7,"user":"bob"}
auto body = crow::json::load(req.body);
```

---

## 11. Concurrency Concepts

### Why Concurrency is Hard — Race Condition Example
```
Thread A: reads is_booked = 0  ← both threads see 0
Thread B: reads is_booked = 0  ←
Thread A: writes is_booked = 1
Thread B: writes is_booked = 1 ← overwrites A, double booking
```
The window between "read" and "write" is called a **critical section**. It must be protected.

### How This Project Solves It
Two layers:

**Layer 1 — InnoDB `SELECT FOR UPDATE`**
Locks the row at DB level. Thread B's `SELECT FOR UPDATE` blocks until Thread A's transaction commits. Thread B then reads `is_booked=1` and returns 409. Works across multiple server instances.

**Layer 2 — Connection Pool Mutex**
Ensures safe concurrent access to the `std::queue<sql::Connection*>`. Without it, two threads could pop the same connection from the queue simultaneously.

### Deadlock Prevention
Only one mutex in this project (`pool_.mutex_`). No two locks → no circular wait → no deadlock possible in the pool code. MySQL-level deadlocks (between transactions locking multiple rows) are handled by InnoDB's deadlock detector.

### Thread-Safe Design Summary
| Shared resource | Protected by |
|---|---|
| `std::queue<sql::Connection*>` | `std::mutex` + `std::condition_variable` |
| MySQL seat row | InnoDB `SELECT FOR UPDATE` |
| MySQL connection itself | One connection per thread (acquired from pool) |

---

## 12. Interview Questions & Answers

---

**Q: Explain this project in 2 sentences.**

A C++ HTTP server that handles seat booking for an event, backed by a MySQL database with a connection pool. The main engineering challenge is preventing double-booking under concurrent requests, solved using InnoDB's `SELECT FOR UPDATE` row-level locking inside explicit transactions.

---

**Q: What is a connection pool and why is it needed?**

Creating a MySQL connection involves a TCP handshake + authentication — roughly 5–10ms overhead. Under high concurrency, creating a new connection per request would saturate the server. A pool pre-creates N connections at startup and reuses them. `acquire()` pops a connection (O(1)); `release()` pushes it back. Wait time drops from 10ms to microseconds.

---

**Q: What happens if all pool connections are in use?**

`acquire()` calls `cv_.wait(lock, predicate)`. The thread releases the mutex and suspends at OS level — zero CPU usage. When any handler finishes and calls `release()`, the destructor of `PooledConnection` fires the stored lambda, which calls `Db::release()`, which pushes the connection back and calls `cv_.notify_one()`. The waiting thread wakes up, re-acquires the mutex, and gets the connection.

---

**Q: Explain `SELECT FOR UPDATE`. Why not just `UPDATE` directly?**

A plain `UPDATE` is atomic at the SQL level but you can't check a condition atomically with it. If you did:
```
Thread A: reads is_booked=0
Thread B: reads is_booked=0
Thread A: UPDATE (succeeds)
Thread B: UPDATE (also succeeds — double booking)
```
`SELECT FOR UPDATE` locks the row before you read it. Thread B's `SELECT FOR UPDATE` blocks until Thread A's transaction commits. Thread B then reads `is_booked=1` and returns 409. The check and update are serialized at the database level.

---

**Q: Why `setAutoCommit(false)` before `SELECT FOR UPDATE`?**

`SELECT FOR UPDATE` only holds its lock within an explicit transaction. With `autoCommit=true`, MySQL commits after every statement — the lock would be released immediately after the `SELECT`, before the `UPDATE`. Another thread could sneak in between. `setAutoCommit(false)` groups the `SELECT FOR UPDATE` + `UPDATE` into one atomic transaction — the lock holds until `commit()`.

---

**Q: What is RAII and where is it used?**

RAII = Resource Acquisition Is Initialization. A C++ pattern where the constructor acquires a resource and the destructor releases it — guaranteed even on exceptions.

Used in three places:
1. `Db` destructor: `delete`s every `sql::Connection*` → MySQL connections closed
2. `PooledConnection` destructor: calls releaser lambda → connection returned to pool
3. `std::unique_ptr<sql::PreparedStatement>`: auto-deletes MySQL statement object

Without RAII, a mid-function exception would skip the cleanup code — connection leaked, pool starved.

---

**Q: Why is `PooledConnection` non-copyable but movable?**

Non-copyable: copying would give two `PooledConnection` objects pointing to the same `sql::Connection*`. When the first destructs, it returns the connection to the pool. The second then also tries to return it — the pool now has a duplicate pointer in its queue, causing corruption on next `acquire()`.

Movable: `db.acquire()` returns by value. The move constructor transfers `conn_` to the caller and sets the source to `nullptr` — the source destructor does nothing. Correct single-ownership semantics.

---

**Q: What is SQL injection and how does this project prevent it?**

SQL injection: attacker embeds SQL in user input to alter query logic. If we built SQL as:
```cpp
"SELECT * FROM seats WHERE id = " + std::to_string(seat_id)
```
a malicious `seat_id = "1 OR 1=1"` returns all seats. With prepared statements:
```cpp
conn->prepareStatement("SELECT * FROM seats WHERE id = ?")
stmt->setInt(1, seat_id);
```
MySQL compiles the query structure first. The parameter is sent separately as typed data — never interpreted as SQL. Injection impossible.

---

**Q: What does `ENGINE=InnoDB` mean in the schema?**

InnoDB is MySQL's default transactional storage engine. It provides:
- Row-level locking (required for `SELECT FOR UPDATE`)
- ACID transactions
- MVCC (readers don't block writers)
- Foreign key enforcement
- Redo log for crash recovery

The alternative, MyISAM, only supports table-level locks — the entire `seats` table would lock on every booking. InnoDB locks only the specific row being booked.

---

**Q: What is MVCC and how does it help here?**

Multi-Version Concurrency Control: InnoDB keeps multiple versions of rows. Plain `SELECT` (without `FOR UPDATE`) reads the last committed snapshot — it never blocks, even if a writer is in the middle of a transaction. This means `GET /seats` (read) and `POST /book` (write) can run concurrently without blocking each other. Only `SELECT FOR UPDATE` requests block on each other.

---

**Q: What happens on a MySQL crash mid-transaction?**

InnoDB uses a redo log — all changes are written to the log before being applied to data pages. If MySQL crashes between `UPDATE` and `COMMIT`, the transaction is incomplete. On restart, InnoDB replays the redo log, sees the uncommitted transaction, and rolls it back. The seat stays as `is_booked=0`. No partial state survives. This is the **Durability** and **Atomicity** part of ACID.

---

**Q: What is a deadlock? Can it happen here?**

A deadlock is circular waiting: Transaction A holds lock on row 1, waits for row 2. Transaction B holds row 2, waits for row 1. Both wait forever. InnoDB detects this (wait-for graph) and rolls back the victim transaction with error 1213.

In this project, each booking transaction locks only one seat row. No transaction holds multiple row locks simultaneously. Deadlock between booking transactions is not possible. If it were, the `catch (sql::SQLException& e)` in the booking handler would catch error 1213 and return a 500.

---

**Q: What does `cv_.notify_one()` do vs `cv_.notify_all()`?**

`notify_one()` wakes exactly one waiting thread. Correct here because only one connection became available — waking all waiting threads would cause them all to try to pop from the queue, all but one would find it empty and go back to sleep. `notify_all()` would be wasteful (thundering herd). `notify_one()` is precise and efficient.

---

**Q: Trace a 409 response end-to-end.**

```
1. User clicks seat B3 (already booked by alice)
2. Browser: POST /book {"event_id":1,"seat_id":12,"user":"bob"}
3. Crow worker thread picks up the request
4. crow::json::load → event_id=1, seat_id=12, user="bob"
5. db.acquire() → pops connection from pool queue
6. conn->setAutoCommit(false)  → BEGIN transaction
7. PreparedStatement: SELECT id, is_booked, seat_label
                      FROM seats WHERE id=12 AND event_id=1 FOR UPDATE
8. InnoDB places exclusive lock on row 12
9. result->next() → row found
10. result->getBoolean("is_booked") → true
11. conn->rollback()          → releases row lock
12. conn->setAutoCommit(true)
13. PooledConnection destructs → lambda fires → Db::release() →
    connection pushed back to queue → cv_.notify_one()
14. return json_response(409,
        {"error":"Seat already booked","seat_label":"B3"})
15. Crow sends HTTP/1.1 409 Conflict + JSON body
16. Browser: res.status === 409 → showToast("Seat B3 already taken!", "error")
```

---

**Q: How would you scale this to multiple server instances?**

With multiple server instances:
- The C++ `std::mutex` in `Db` only protects within one process — no effect across processes
- BUT `SELECT FOR UPDATE` works globally — InnoDB row locks are enforced at the database level regardless of which server instance issues the query

So horizontal scaling (multiple C++ processes behind a load balancer) works correctly for booking correctness. The connection pool just means each instance maintains its own N connections to MySQL.

For read scalability, add MySQL read replicas — route `GET /seats` to replicas, write operations to primary.

---

**Q: Why is `std::unique_lock` used in `acquire()` but `std::lock_guard` in `release()`?**

`condition_variable::wait()` requires a `std::unique_lock` because it needs to atomically release the mutex and sleep — then re-acquire on wake. `lock_guard` cannot release mid-scope.

`release()` just pushes to the queue and notifies — no waiting needed. `lock_guard` is simpler, has less overhead, and is sufficient.

---

*End of Interview Prep — MySQL Edition v2.0*
