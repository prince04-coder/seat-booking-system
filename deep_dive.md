# Deep Dive — Everything About This Project

> Every technology, every decision, every risk, every "what if" scenario.
> Written assuming you know nothing. Read top to bottom.

---

# SECTION 1 — C++ : Why This Language?

## What C++ Is

C++ is a compiled, statically typed, systems programming language.
"Compiled" means your code is translated into machine instructions before it runs.
There is no interpreter running in between (unlike Python or JavaScript).
The CPU executes your code directly.

## Why C++ Was Chosen Here

**Reason 1 — Threads are explicit.**
In C++, you create threads manually. You manage mutexes manually.
Nothing is hidden. This forces you to understand exactly what is happening
with concurrency — which is the entire point of this project.

**Reason 2 — No garbage collector.**
Languages like Java and Go have a garbage collector (GC) that automatically
frees memory. But the GC can pause your program at random moments to clean up.
Under high load, a GC pause of even 20ms can cause a queue of requests to pile up.
C++ has no GC. You control memory explicitly. Latency is predictable.

**Reason 3 — Performance ceiling.**
C++ is one of the fastest languages for CPU-bound and memory-bound work.
For a server handling thousands of concurrent requests, this matters.

**Reason 4 — RAII.**
C++ has a feature called RAII (explained in full later) that makes resource
management safe and automatic through object lifetimes. It is the perfect
pattern for connection pools.

## Why Not Other Languages?

**Python:** Much slower. The GIL (Global Interpreter Lock) prevents true parallel
execution of Python threads. You'd need multiprocessing which is heavier.
Also, Python's dynamic typing means more runtime errors.

**Go:** Go is actually a very good choice for this problem. Its goroutines
and channels handle concurrency elegantly. But Go has a GC (with stop-the-world
pauses) and the learning goal here was explicit thread/mutex management.

**Java:** JVM startup overhead, GC pauses, and higher memory usage per thread.
The JVM also abstracts away the low-level details this project is meant to teach.

**Node.js:** Single-threaded event loop. Excellent for I/O-bound tasks but
not ideal for demonstrating explicit thread-level concurrency.

## C++17 Specifically

This project uses C++17 (not C++11, C++14, or C++20). Why 17?

- `std::optional` available (could be used for nullable returns)
- Structured bindings (cleaner code)
- `if constexpr` for compile-time branching
- Widely supported by all major compilers (GCC, Clang, MSVC)
- C++20 features (coroutines, modules) were not needed here

---

# SECTION 2 — MySQL : Why This Database?

## What MySQL Is

MySQL is a relational database management system (RDBMS).
"Relational" means data is stored in tables with rows and columns,
and tables can relate to each other via foreign keys.

MySQL is a CLIENT-SERVER database. There is:
- A MySQL server process that runs all the time
- Clients (our C++ code) that connect to it over TCP

## The Most Important Part: InnoDB Engine

MySQL supports multiple "storage engines" — different ways of storing data.
Our schema uses `ENGINE=InnoDB`. This is critical.

```sql
CREATE TABLE seats (...) ENGINE=InnoDB;
```

InnoDB gives us:

**1. Row-level locking**
When you lock a row, only THAT row is locked.
Other rows in the same table are untouched.
Without InnoDB (e.g., MyISAM), the ENTIRE TABLE locks on every write.
That means if 100 people are booking different seats, they'd all wait for each other.
With InnoDB, booking seat A1 doesn't block booking seat B5 at all.

**2. Transactions (BEGIN/COMMIT/ROLLBACK)**
InnoDB supports proper ACID transactions.
MyISAM does not — no rollback, no atomicity.

**3. SELECT FOR UPDATE**
This specific SQL statement only works with InnoDB.
It is the entire mechanism that prevents double booking.
More on this later.

**4. MVCC (Multi-Version Concurrency Control)**
InnoDB keeps multiple versions of rows.
A plain SELECT reads the last committed snapshot without blocking.
This means reading seats (GET /seats) never blocks someone booking (POST /book).

**5. Redo log / crash recovery**
InnoDB writes changes to a redo log before applying them.
If the server crashes mid-transaction, on restart InnoDB replays the log
and rolls back any incomplete transactions automatically.

## Why Not PostgreSQL?

PostgreSQL is technically superior in many ways:
- Better standards compliance
- More advanced query planner
- Better support for JSON, arrays, full-text search
- Also has row-level locking and MVCC

However:
- The C++ driver ecosystem for PostgreSQL (libpqxx) is less mature
- mysql-connector-cpp via vcpkg is more straightforward to set up
- For the SQL features this project uses (transactions, FOR UPDATE), MySQL and PostgreSQL are equivalent
- MySQL 8.0 closed most of the gap with recursive CTEs and window functions

In production you could swap MySQL for PostgreSQL and the SQL would be nearly identical.

## Why Not SQLite?

SQLite is a file-based database — no server needed.
It is simpler to set up, but:
- SQLite uses file-level locking, not row-level locking
- Concurrent writes are serialized (one writer at a time)
- Not suitable for production multi-user concurrent booking
- Would not teach the same concurrency concepts

We specifically need MySQL InnoDB's row-level locking for this project.

## Why Not MongoDB or Other NoSQL?

MongoDB stores documents, not rows. It doesn't have:
- Traditional transactions (added later, but limited)
- `SELECT FOR UPDATE` style locking
- ACID guarantees by default

For a booking system where correctness is mandatory, a relational database
with proper transactions is the right choice.

## MySQL Connection Over TCP

```cpp
url_ = "tcp://" + host + ":" + std::to_string(port);
// → "tcp://127.0.0.1:3306"
```

Our C++ code connects to MySQL over TCP/IP on port 3306.
Even though MySQL is on the same machine (via Docker), we still use TCP
because the MySQL server runs in a separate network namespace inside Docker.

Each `driver_->connect(url_, user_, password_)` call:
1. Opens a TCP socket to 127.0.0.1:3306
2. Performs MySQL handshake (sends client version, auth)
3. Sends `USE ticketing` (setSchema)
4. Connection is ready

This takes ~5–10ms. That is why we create the pool at startup
and reuse connections instead of creating one per request.

---

# SECTION 3 — Crow HTTP Framework : Why This Library?

## What Crow Is

Crow is a C++ micro web framework inspired by Python's Flask.
It handles:
- Listening on a TCP port for HTTP connections
- Parsing HTTP requests (method, path, headers, body)
- Routing requests to the correct handler function
- Building and sending HTTP responses
- Managing a thread pool for concurrent request handling

## Why Crow?

**Header-only (mostly):** Most of Crow is in `.h` files. Easy to include.

**Minimal:** Does not force an architecture on you. You write the handlers,
Crow handles the HTTP plumbing.

**Built-in JSON:** `crow::json::load()` and `crow::json::wvalue` handle
parsing and building JSON without a separate library.

**Thread pool built in:** `.concurrency(4)` creates 4 worker threads.
Crow distributes incoming connections across them automatically.

**CROW_ROUTE macro:** Registers a URL pattern and its handler cleanly:
```cpp
CROW_ROUTE(app, "/seats")
([&db](const crow::request& req) {
    // handle GET /seats
});
```

## Why Not Other Options?

**Boost.Beast:** Very powerful but extremely verbose. Even a simple HTTP
server requires hundreds of lines of boilerplate with Beast.

**cpp-httplib:** Simpler than Crow but single-threaded by default.
Would need extra work for concurrent handling.

**Pistache:** Good library but less actively maintained and harder to
set up on macOS with vcpkg.

**Writing raw sockets:** Possible, educational, but not the point of
this project. We want to focus on the database/concurrency logic.

## How Crow's Thread Pool Works

```cpp
app.port(18080).concurrency(4).run();
```

This creates:
- 1 main thread that accepts incoming TCP connections
- 4 worker threads that each handle one request at a time

When a request comes in:
1. Main thread accepts the TCP connection
2. Assigns it to whichever worker thread is free
3. Worker thread calls the matching route handler
4. Handler runs, builds response, sends it back
5. Worker thread is free again

All 4 worker threads share the same `Db` object.
This is why thread safety (mutex, condition_variable) is needed.

## What SimpleApp Means

```cpp
crow::SimpleApp app;
```

Crow has two app types:
- `crow::App<Middleware1, Middleware2>` — with middleware pipeline
- `crow::SimpleApp` — no middleware, just routes

We use SimpleApp because we don't need middleware here.
The middleware version uses template metaprogramming (variadic templates)
to compose middlewares at compile time with zero runtime overhead.

---

# SECTION 4 — Connection Pool : Deep Technical Detail

## Why Connections Are Expensive

Creating a MySQL connection involves:
1. DNS resolution (if using hostname)
2. TCP 3-way handshake (SYN → SYN-ACK → ACK)
3. MySQL server sends greeting packet
4. Client sends auth packet (username, hashed password)
5. Server validates credentials
6. Client sends `USE ticketing` (setSchema)
7. Server acknowledges

Total time: 5–15ms depending on network.

If you have 1000 requests per second and create a new connection per request:
1000 × 10ms = 10 seconds worth of connection overhead per second.
Your server would fall further and further behind.

## The Pool Solution

Create N connections once at startup. Reuse them.

```cpp
// Constructor — runs ONCE at startup
for (size_t i = 0; i < pool_size_; ++i) {
    pool_.push(create_connection());
}
```

Now every request just pops from the queue — microseconds, not milliseconds.

## The std::queue Choice

`std::queue` is a FIFO (First In, First Out) container.
Connections are popped from the front, pushed to the back.
This means connections get used in round-robin fashion,
preventing any single connection from being overused.

```
Push order:  conn1, conn2, conn3 ... conn10
Pop order:   conn1 first, then conn2, etc.
After return: conn1 goes to the back
Next pop:    conn2
```

This distributes wear evenly across all connections.

## std::mutex — The Guard

The queue is shared by all 4 Crow worker threads.
Without protection, two threads could pop the same connection simultaneously.

`std::mutex` is a mutual exclusion primitive.
Only one thread can hold the mutex at a time.

```cpp
// acquire() — simplified
std::unique_lock<std::mutex> lock(mutex_);  // LOCK — others wait here
sql::Connection* conn = pool_.front();       // safe to access queue
pool_.pop();
// lock releases when unique_lock goes out of scope
```

At the hardware level, `mutex.lock()` uses atomic CPU instructions
(like CMPXCHG on x86) to guarantee only one thread succeeds.
Other threads spin briefly, then the OS deschedules them if they wait too long.

## std::condition_variable — The Sleep Mechanism

What happens when all 10 connections are in use and an 11th request arrives?

**Bad approach (busy waiting):**
```cpp
while (pool_.empty()) { /* spin */ }  // burns 100% CPU doing nothing
```

**Good approach (condition_variable):**
```cpp
cv_.wait(lock, [this] { return !pool_.empty(); });
```

`cv_.wait()` does three things atomically:
1. Releases the mutex (so other threads can push connections back)
2. Suspends the current thread (OS removes it from CPU scheduler)
3. When woken by `cv_.notify_one()`, re-acquires the mutex and continues

The thread uses ZERO CPU while sleeping. The OS wakes it only when needed.

## std::unique_lock vs std::lock_guard

`acquire()` uses `std::unique_lock`. Why not `lock_guard`?

`cv_.wait()` needs to release the mutex while sleeping.
`lock_guard` cannot release mid-scope — it holds until destroyed.
`unique_lock` can release and re-acquire, which is what `cv_.wait()` does internally.

`release()` uses `lock_guard` because it just needs simple lock/unlock.
No sleeping, no releasing mid-scope needed.

## PooledConnection — The RAII Wrapper

```cpp
class PooledConnection {
    sql::Connection* conn_;
    std::function<void(sql::Connection*)> releaser_;
public:
    ~PooledConnection() {
        if (conn_ && releaser_) releaser_(conn_);  // returns to pool
    }
};
```

The `releaser_` is a lambda stored as a `std::function`:
```cpp
return PooledConnection(conn, [this](sql::Connection* c) { this->release(c); });
```

When `PooledConnection` is destroyed (goes out of scope), its destructor
calls `releaser_(conn_)`, which calls `Db::release(conn)`,
which pushes the connection back and calls `cv_.notify_one()`.

This happens automatically. You cannot forget to return a connection.

## Why Non-Copyable?

```cpp
PooledConnection(const PooledConnection&) = delete;
```

If you could copy a `PooledConnection`, two objects would hold the same `conn_` pointer.
When the first destructs, it returns the connection to the pool.
Now the second destructs — it tries to return the same connection again.
The pool now has a duplicate entry. Next `acquire()` pops the same connection twice
and gives it to two different handlers simultaneously — they share one MySQL connection,
interleave their SQL statements, corrupt each other's data.

`= delete` makes this a compile-time error. Impossible to make the mistake.

## Why Movable?

```cpp
PooledConnection(PooledConnection&& other) noexcept
    : conn_(other.conn_), releaser_(std::move(other.releaser_)) {
    other.conn_     = nullptr;
    other.releaser_ = nullptr;
}
```

`db.acquire()` returns a `PooledConnection` by value.
Move semantics transfer ownership: the source's `conn_` becomes `nullptr`
so its destructor does nothing. The destination now owns the connection.
This is zero-copy transfer of ownership — efficient and safe.

---

# SECTION 5 — SELECT FOR UPDATE : The Core of Everything

## The Problem This Solves

Two threads, same seat, same millisecond:

```
Thread A: reads row 7 → is_booked = 0   (free!)
Thread B: reads row 7 → is_booked = 0   (free! — before A wrote)
Thread A: UPDATE row 7 SET is_booked = 1
Thread B: UPDATE row 7 SET is_booked = 1  ← also succeeds. Double booking.
```

The gap between "read" and "write" is called a TOCTOU race
(Time Of Check To Time Of Use). Both threads checked at the same time,
both saw it was free, both wrote. MySQL accepted both writes — each was
valid when it arrived.

## What SELECT FOR UPDATE Does

```sql
SELECT id, is_booked, seat_label
FROM seats
WHERE id = 7 AND event_id = 1
FOR UPDATE
```

The `FOR UPDATE` clause tells InnoDB:
"Give me this row AND put an exclusive lock on it right now."

An exclusive lock means:
- No other transaction can put ANY lock on this row (shared or exclusive)
- No other transaction can UPDATE this row
- No other transaction can SELECT FOR UPDATE this row

Any transaction that tries will BLOCK — it literally pauses execution
and waits until the first transaction commits or rolls back.

## Why It Must Be Inside a Transaction

```cpp
conn->setAutoCommit(false);   // BEGIN TRANSACTION

// SELECT FOR UPDATE — lock acquired here
// ...
// UPDATE — modify the row
conn->commit();               // COMMIT — lock released HERE
```

If `autoCommit` were true, the lock would be released immediately
after the SELECT FOR UPDATE statement. Before the UPDATE could run,
another thread could sneak in, see `is_booked = 0`, and also book it.

The lock must be held from SELECT until COMMIT. That's only possible
inside an explicit transaction.

## InnoDB Lock Types

InnoDB has several lock types:

```
Shared lock (S):    Multiple transactions can hold simultaneously
                    Used by: SELECT ... LOCK IN SHARE MODE
                    Purpose: "I want to read this row and prevent writes"

Exclusive lock (X): Only ONE transaction can hold
                    Used by: SELECT ... FOR UPDATE, UPDATE, DELETE
                    Purpose: "I want to modify this row, nobody else touch it"

Intention locks:    Table-level locks that signal intent
                    InnoDB manages these automatically
```

`FOR UPDATE` acquires an exclusive (X) lock on matched rows.
No other transaction can acquire S or X lock on those rows until release.

## MVCC: Why Reads Don't Block

Plain `SELECT` (without FOR UPDATE) uses MVCC (Multi-Version Concurrency Control).

InnoDB keeps multiple versions of each row:
- The currently committed version
- Older versions in the undo log

When Thread A holds an exclusive lock on row 7 and is updating it,
Thread B does a plain `SELECT * FROM seats WHERE event_id = 1`:
- Thread B reads the last COMMITTED version of row 7
- It does NOT see Thread A's uncommitted change
- It does NOT wait for Thread A's lock

This is why GET /seats never blocks while bookings are happening.
Readers and writers are fully concurrent — readers never block writers,
writers never block readers. Only writer-writer conflicts block.

## Optimistic vs Pessimistic Locking

There are two philosophies for handling concurrent modifications:

**Optimistic Locking (not used here)**
Assume conflicts are rare. Don't lock upfront.
Add a `version` column to each row.
When updating: check that version hasn't changed since you read.
If it has, someone else modified it — retry.

```sql
-- Read
SELECT id, is_booked, version FROM seats WHERE id = 7;
-- returns: is_booked=0, version=5

-- Update (only succeeds if version is still 5)
UPDATE seats SET is_booked=1, version=6 WHERE id=7 AND version=5;
-- if 0 rows affected → someone else changed it → retry
```

Problem: Under high contention (many people clicking same seat),
most updates fail and retry repeatedly — "thundering herd."
At 1000 concurrent requests, almost all fail and retry → server melts.

**Pessimistic Locking (what we use)**
Assume conflicts are likely. Lock the row BEFORE checking.
First one to lock it wins. Others wait. No retries needed.

```sql
SELECT ... FOR UPDATE  ← lock upfront
```

For a ticketing system where the same popular seats get hammered simultaneously,
pessimistic locking is the correct choice. Losers wait once, get a clean 409.
No retries, no thundering herd.

---

# SECTION 6 — ACID Transactions : What Each Letter Means in This Project

## A — Atomicity

All steps in a transaction succeed, or none do.

In our booking:
```
BEGIN
  SELECT FOR UPDATE    step 1
  UPDATE seat          step 2
COMMIT (or ROLLBACK)
```

If the server crashes after the UPDATE but before COMMIT:
- InnoDB's redo log has the UPDATE recorded
- But since COMMIT never happened, InnoDB sees an incomplete transaction
- On restart, InnoDB rolls it back automatically
- Seat stays as is_booked=0 — as if the booking never happened
- The user's request failed (they'd get a network error) but no data corruption

If there's a MySQL error during UPDATE:
```cpp
} catch (...) {
    conn->rollback();  // undo the SELECT FOR UPDATE effects
    conn->setAutoCommit(true);
    throw;
}
```
Rollback undoes everything. The seat stays unbooked.

## C — Consistency

The database must be in a valid state before AND after every transaction.

Our schema has constraints that enforce consistency:

```sql
UNIQUE KEY unique_seat (event_id, seat_label)
```
Even if two transactions somehow both tried to insert the same seat,
the UNIQUE constraint would reject the second one with a duplicate key error.
This is a backup safety net at the database level.

```sql
FOREIGN KEY (event_id) REFERENCES events(id) ON DELETE CASCADE
```
You cannot insert a seat for event_id=999 if event 999 doesn't exist.
The database rejects it. Consistency enforced.

```sql
is_booked TINYINT(1) NOT NULL DEFAULT 0
```
is_booked can never be NULL. Always 0 or 1. No ambiguous state.

## I — Isolation

Concurrent transactions don't see each other's incomplete work.

Without isolation: Thread B could read Thread A's partially-completed booking
and make decisions based on data that might be rolled back.

InnoDB defaults to REPEATABLE READ isolation level.
With our SELECT FOR UPDATE, we get SERIALIZABLE behavior for the affected rows:
- Thread B literally cannot even SEE the lock-contested row until Thread A commits
- Thread B's SELECT FOR UPDATE blocks at the MySQL level, not the application level
- Once Thread A commits, Thread B proceeds with the fully committed state

## D — Durability

Once committed, data survives crashes.

InnoDB's redo log (also called the write-ahead log / WAL):
1. Before writing a page to disk, InnoDB writes the change to the redo log
2. On COMMIT, the redo log entry is flushed to disk (fsync)
3. MySQL acknowledges the commit to the client
4. The actual data page is written to disk later (asynchronously)

If the system crashes after step 2 but before step 4:
- On restart, InnoDB replays the redo log
- The change is reapplied to the data page
- Data is not lost

The redo log is append-only and sequential — much faster than random writes
to data pages. This is why InnoDB can promise durability without killing performance.

---

# SECTION 7 — Prepared Statements : Security and Performance

## The SQL Injection Attack

If you build SQL by concatenating user input:

```cpp
// DANGEROUS — never do this
string sql = "SELECT * FROM seats WHERE event_id = " + req.url_params.get("event_id");
```

A hacker sends: `event_id = 0 UNION SELECT password,username,NULL,NULL FROM mysql.user`

The SQL becomes:
```sql
SELECT * FROM seats WHERE event_id = 0
UNION SELECT password,username,NULL,NULL FROM mysql.user
```

The hacker just stole all MySQL usernames and password hashes.

Or they send: `event_id = 1; DROP TABLE seats; --`

```sql
SELECT * FROM seats WHERE event_id = 1; DROP TABLE seats; --
```

50 seats, gone.

## How Prepared Statements Work

```cpp
// 1. Send the SQL template to MySQL — it compiles the query plan
auto stmt = conn->prepareStatement(
    "SELECT * FROM seats WHERE event_id = ?"
);

// 2. Bind the parameter as typed data — sent separately from the SQL
stmt->setInt(1, event_id);

// 3. Execute
auto result = stmt->executeQuery();
```

MySQL receives two separate things:
- The SQL structure (step 1): "select all seats where event_id equals a parameter"
- The parameter value (step 2): the integer 1

The value is NEVER interpreted as SQL. It is data.
If someone sends `event_id = "1 OR 1=1"`, MySQL stores it as the string "1 OR 1=1"
and tries to compare it to an integer column. It either returns 0 rows or an error.
No injection possible.

## Performance Benefit

When MySQL receives a prepared statement:
1. It parses the SQL
2. Builds an execution plan (which index to use, join order, etc.)
3. Compiles it into an internal representation

This plan is cached. The next time you call `stmt->setInt(1, newValue)` and
`executeQuery()`, MySQL skips steps 1–3 and executes the cached plan directly.

For repeated queries (like fetching seats many times), this is faster.

---

# SECTION 8 — The Schema Design Decisions

## Why TINYINT(1) for is_booked?

```sql
is_booked TINYINT(1) NOT NULL DEFAULT 0
```

MySQL doesn't have a true BOOLEAN type. `TINYINT(1)` is the conventional
representation: 0 = false, 1 = true.

`NOT NULL` — we never want this to be ambiguous (NULL means "unknown").
`DEFAULT 0` — new seats start as unbooked automatically.

## Why UNIQUE KEY on (event_id, seat_label)?

```sql
UNIQUE KEY unique_seat (event_id, seat_label)
```

This prevents inserting duplicate seats for the same event.
"A1" can exist in event 1 AND event 2 — that's fine (different events).
But two "A1" rows in event 1 is impossible — the DB rejects it.

This is a safety net. Even if application code had a bug and tried to
insert a duplicate seat, the database would catch it and throw an error.

## Why INDEX idx_event?

```sql
INDEX idx_event (event_id)
```

Without this index, `SELECT ... WHERE event_id = 1` would scan every row
in the seats table (full table scan). With the index, MySQL jumps directly
to the rows for event_id=1.

For 50 seats this makes no difference. For 50,000 seats across 1000 events,
the difference is massive — O(n) vs O(log n) lookup.

## Why FOREIGN KEY with ON DELETE CASCADE?

```sql
FOREIGN KEY (event_id) REFERENCES events(id) ON DELETE CASCADE
```

This creates a relationship between the seats and events tables.
Two effects:

1. You cannot insert a seat for an event that doesn't exist.
   MySQL rejects it: "Cannot add or update a child row: a foreign key constraint fails."

2. If you delete event 1, all seats where event_id=1 are automatically deleted.
   No orphaned seat records that reference a non-existent event.

## Why Recursive CTE for Seeding?

```sql
WITH RECURSIVE nums AS (
    SELECT 0 AS n
    UNION ALL
    SELECT n + 1 FROM nums WHERE n < 49
)
SELECT 1, CONCAT(CHAR(65 + (n DIV 10) USING utf8mb4), CAST((n MOD 10) + 1 AS CHAR))
FROM nums;
```

This generates 50 rows (n=0 to n=49) in pure SQL.
- n=0:  CHAR(65 + 0) = 'A', 0 MOD 10 + 1 = 1  → "A1"
- n=10: CHAR(65 + 1) = 'B', 10 MOD 10 + 1 = 1 → "B1"
- n=49: CHAR(65 + 4) = 'E', 49 MOD 10 + 1 = 10 → "E10"

`INSERT IGNORE` silently skips rows that would violate the UNIQUE KEY.
Safe to run multiple times — it won't create duplicates.

Requires MySQL 8.0+. Earlier MySQL versions didn't support recursive CTEs.

---

# SECTION 9 — Docker : Why and How

## What Docker Is

Docker is a containerization platform.
A container is an isolated process with its own filesystem, network, and processes.
It's like a lightweight virtual machine, but shares the host OS kernel.

## Why Docker for MySQL?

Without Docker, to run MySQL you'd need to:
1. Download the MySQL installer
2. Run it, set root password
3. Configure my.cnf
4. Start the MySQL service
5. Create the database and user manually
6. Apply the schema manually

With Docker:
```
docker compose up -d
```
One command. MySQL is running in 30 seconds with all configuration applied.

## docker-compose.yml Explained

```yaml
services:
  mysql:
    image: mysql:8.0              # use the official MySQL 8.0 image from Docker Hub
    container_name: ticketing_mysql
    environment:
      MYSQL_ROOT_PASSWORD: root   # root password
      MYSQL_DATABASE: ticketing   # create this database automatically
      MYSQL_USER: ticketing       # create this user automatically
      MYSQL_PASSWORD: ticketing   # with this password
    ports:
      - "3306:3306"               # host port 3306 → container port 3306
    volumes:
      - mysqldata:/var/lib/mysql  # persist data here — survives container restarts
      - ./sql/schema.sql:/docker-entrypoint-initdb.d/01-schema.sql
      #  ↑ This file is run automatically when the container first starts
```

The `docker-entrypoint-initdb.d/` directory is special.
MySQL's Docker entrypoint script runs all `.sql` files in this directory
on first initialization. So our schema is applied automatically.

`mysqldata` is a Docker volume — data persisted on the host machine.
If you `docker compose down` and `docker compose up` again,
your bookings are still there.

## Port Mapping: 3306:3306

Our C++ code connects to `127.0.0.1:3306`.
Docker maps that to port 3306 inside the container where MySQL is actually listening.
This is network address translation at the Docker level.

---

# SECTION 10 — RAII : Why Resources Never Leak

## The Problem RAII Solves

In C, you manage memory and resources manually:

```c
FILE* f = fopen("file.txt", "r");
// ... do stuff ...
// What if an error happens here before fclose?
fclose(f);  // might never be called → file handle leaked
```

If there's an early return, an exception, or a crash between open and close,
the resource leaks. In a server that runs for weeks, leaked connections
accumulate until the pool is exhausted.

## How RAII Works

Tie the resource to an object's lifetime.
Constructor acquires the resource.
Destructor releases it.
C++ GUARANTEES the destructor runs when the object goes out of scope —
even if an exception is thrown.

```cpp
// lock_guard example
{
    std::lock_guard<std::mutex> lock(mutex_);  // mutex locked
    pool_.push(conn);
    cv_.notify_one();
}   // lock_guard destructs HERE — mutex unlocked automatically
    // Even if push() or notify_one() threw an exception
```

## Every RAII Object in This Project

**std::lock_guard<std::mutex>**
```
Created  → mutex.lock()
Destroyed → mutex.unlock()
Where: release(), available(), Db destructor
```

**std::unique_lock<std::mutex>**
```
Created  → mutex.lock()
During wait → mutex.unlock() (sleeping) → mutex.lock() (woken)
Destroyed → mutex.unlock()
Where: acquire()
```

**PooledConnection**
```
Created  → connection popped from pool
Destroyed → connection pushed back to pool + cv_.notify_one()
Where: every route handler that calls db.acquire()
```

**std::unique_ptr<sql::PreparedStatement>**
```
Created  → holds pointer to PreparedStatement object
Destroyed → delete preparedStatement (frees MySQL driver memory)
Where: every route handler
```

**std::unique_ptr<sql::ResultSet>**
```
Created  → holds pointer to ResultSet object
Destroyed → delete resultSet (frees result memory)
Where: GET /seats, POST /book
```

**Db class itself**
```
Created  → creates N MySQL connections
Destroyed → deletes all MySQL connections (program exit)
Where: main.cpp stack — destructs when main() returns
```

## What Happens on Exception

```cpp
auto conn = db.acquire();              // PooledConnection created (conn popped)
conn->setAutoCommit(false);
auto stmt = std::unique_ptr<...>(conn->prepareStatement(...));
stmt->setInt(1, seat_id);
auto result = std::unique_ptr<...>(stmt->executeQuery());  // ← MySQL throws here!

// EXCEPTION PROPAGATES UPWARD
// Stack unwinds:
// result unique_ptr destructs → ResultSet deleted
// stmt unique_ptr destructs → PreparedStatement deleted
// conn PooledConnection destructs → release() called → connection back to pool
// catch block in outer try catches the sql::SQLException
// Returns 500 to client
// Pool is healthy. No leaks. Server continues.
```

Without RAII, the exception would skip the manual cleanup code and
the connection would be lost from the pool permanently.
After enough exceptions, the pool would be empty and all requests would hang forever.

---

# SECTION 11 — CRASH SCENARIOS : What Happens When Things Go Wrong

## Scenario 1: C++ Server Crashes Mid-Booking (between UPDATE and COMMIT)

```
Thread A: setAutoCommit(false)
Thread A: SELECT seat 7 FOR UPDATE  → row locked
Thread A: UPDATE seat 7 SET is_booked=1
[SERVER CRASHES HERE — power failure, segfault, OOM killer]
```

**What happens to MySQL:**
- MySQL is still running (it's in Docker, separate process)
- The transaction was never committed
- InnoDB detects the disconnected client
- MySQL automatically rolls back the transaction
- Seat 7 returns to is_booked=0

**What the user sees:**
- Network connection error / timeout
- Their booking failed
- They can try again — seat 7 is available

**What happens when server restarts:**
- New `Db` constructor runs
- Creates 10 fresh MySQL connections
- Everything starts clean

**Data integrity:** PRESERVED. No double booking, no ghost booking.

## Scenario 2: MySQL Crashes Mid-Transaction

```
Thread A: setAutoCommit(false)
Thread A: SELECT seat 7 FOR UPDATE
Thread A: UPDATE seat 7 SET is_booked=1
[MYSQL CRASHES HERE]
```

**What InnoDB does on restart:**
1. Reads the redo log
2. Sees an UPDATE for seat 7 that was never followed by a COMMIT
3. Rolls it back automatically (crash recovery)
4. Seat 7 is is_booked=0

**What the C++ server does:**
- All 10 pooled connections throw `sql::SQLException` (lost connection)
- `acquire()` tries `conn->isClosed()` → true → `delete conn; create_connection()`
- But MySQL is down, so `create_connection()` throws
- The server throws during pool creation → requests get 500 errors
- `GET /health` returns 503

**Recovery:**
- MySQL restarts (Docker: `docker compose restart mysql`)
- C++ server's stale connections detect failure on next `acquire()`
- `isClosed()` check → reconnect → new connection created
- Server recovers automatically without restart

## Scenario 3: All 10 Pool Connections Are Busy — New Request Arrives

```
Request 11 arrives
db.acquire() called
std::unique_lock lock(mutex_)
cv_.wait() — pool is empty
Thread sleeps — uses 0 CPU
...
Request 3 finishes → PooledConnection destructs → release() → cv_.notify_one()
Thread 11 wakes up → acquires connection → proceeds normally
```

**What the user sees:** slightly higher response time (waited for a connection)
**Data integrity:** PRESERVED
**Server behavior:** correct — no errors, just queuing

If requests pile up faster than they complete, the queue grows.
Eventually Crow's internal TCP accept queue fills up and new connections
are refused with "connection refused". This is graceful degradation.

## Scenario 4: MySQL Closes Idle Connection (wait_timeout)

MySQL has a `wait_timeout` setting (default: 8 hours).
If a connection sits idle for that long, MySQL closes it server-side.

Our pool has this connection in its queue, doesn't know it's dead.
Next `acquire()` pops it:

```cpp
if (conn->isClosed()) {
    std::cout << "[Db] Reconnecting stale connection..." << std::endl;
    delete conn;
    conn = create_connection();  // fresh connection
}
```

`isClosed()` detects the dead connection.
We delete it and create a new one transparently.
The route handler gets a valid connection and never knows this happened.

**What the user sees:** nothing — seamless

## Scenario 5: Network Partition Between Server and MySQL

The C++ server is running. MySQL is running. But the network between them fails.

**During an active booking:**
- `stmt->executeQuery()` or `conn->commit()` throws `sql::SQLException`
- Caught by `catch(...)` block
- `conn->rollback()` attempted (may also fail)
- `conn->setAutoCommit(true)` attempted
- 500 returned to user
- PooledConnection destructs — `conn->isClosed()` will return true on next use

**Next request using this connection:**
- `isClosed()` detected in `acquire()`
- Connection deleted and recreated
- New connection either succeeds (network back) or fails (network still down)

**If network stays down:**
- All `create_connection()` calls throw
- Pool becomes empty
- `cv_.wait()` never gets notified (connections never return to pool)
- All requests hang at `db.acquire()`

**What should be added in production:** a timeout on `cv_.wait()`:
```cpp
cv_.wait_for(lock, std::chrono::seconds(5), [this] { return !pool_.empty(); });
```
After 5 seconds, return an error instead of hanging forever.

## Scenario 6: Two Transactions Deadlock

A deadlock happens when:
- Transaction A locks row 1, wants row 2
- Transaction B locks row 2, wants row 1
- Both wait forever

**Can this happen in our project?**
Each booking transaction locks exactly ONE row (the requested seat).
No transaction holds multiple seat locks simultaneously.
Therefore, a classical deadlock between booking transactions is impossible.

However, if MySQL's internal lock manager has a bug, or if a future
feature adds multi-row operations, InnoDB's deadlock detector would:
1. Detect the circular wait-for graph
2. Pick a "victim" (usually the transaction that has done less work)
3. Roll it back with error code 1213 (ER_LOCK_DEADLOCK)

Our catch block handles this:
```cpp
catch (const sql::SQLException& e) {
    // error 1213 lands here
    return json_response(500, R"({"error":"Database error"})");
}
```
In production, you'd check `e.getErrorCode() == 1213` and retry automatically.

## Scenario 7: Memory Leak If Exception in Pool Constructor

```cpp
Db::Db(...) {
    for (size_t i = 0; i < pool_size_; ++i) {
        try {
            pool_.push(create_connection());  // what if this throws on i=5?
        } catch (const sql::SQLException& e) {
            throw;  // propagates out
        }
    }
}
```

If `create_connection()` throws on the 6th connection (connections 1–5 already created):
- The exception propagates out of the constructor
- `Db` object is never fully constructed
- `Db`'s destructor DOES NOT RUN (C++ rule: destructor only runs for fully constructed objects)
- Connections 1–5 are leaked!

**This is a real bug in the current code.**

The fix is to catch the exception, clean up existing connections, then re-throw:
```cpp
} catch (...) {
    while (!pool_.empty()) { delete pool_.front(); pool_.pop(); }
    throw;
}
```
Or use a vector of `unique_ptr<sql::Connection>` instead of raw pointers,
so they self-destruct if the constructor throws.

## Scenario 8: Server Runs Out of Memory (OOM)

If memory usage grows to the point where the OS OOM killer activates:
- The OOM killer sends SIGKILL to the process
- SIGKILL cannot be caught or ignored
- Process terminates immediately — no destructors run
- All RAII cleanup does NOT happen on SIGKILL

**MySQL transactions:** Rolled back automatically (same as Scenario 2)
**Pool connections:** MySQL detects disconnected clients, releases locks
**Data integrity:** PRESERVED (MySQL handles it)

But this highlights that RAII is not a substitute for external reliability.
In production: monitor memory usage, set appropriate limits,
use process supervisors (systemd, Kubernetes) to auto-restart.

---

# SECTION 12 — CORS : Why We Add Those Headers

## What CORS Is

CORS = Cross-Origin Resource Sharing.

A browser's security model blocks JavaScript from making HTTP requests
to a different "origin" (scheme + hostname + port) than the page was loaded from.

Example: Your page is loaded from `http://localhost:3000` (a React dev server).
It tries to call `http://localhost:18080/book`.
The ports are different → different origin → browser BLOCKS the request.

## The OPTIONS Preflight

Before the real POST /book, the browser sends:
```
OPTIONS /book HTTP/1.1
Origin: http://localhost:3000
Access-Control-Request-Method: POST
Access-Control-Request-Headers: Content-Type
```

This is asking: "Hey server, will you allow a POST from localhost:3000?"

Our handler responds:
```
HTTP/1.1 204 No Content
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: POST, OPTIONS
Access-Control-Allow-Headers: Content-Type
```

"Yes, I allow POST from any origin."

The browser then sends the real POST request.

## Why * (Allow All Origins)?

In development, `*` is convenient. In production, you'd restrict this:
```cpp
res.set_header("Access-Control-Allow-Origin", "https://yourapp.com");
```

Using `*` in production means any website can make requests to your API.
For a public API that's fine. For an internal admin API, it's a security risk.

---

# SECTION 13 — CMake and vcpkg : The Build System

## What CMake Does

CMake is a meta-build system. It generates platform-specific build files.

```
CMakeLists.txt → cmake → Makefile (Linux) or .sln (Windows)
                Makefile → make → ticketing_engine binary
```

Our `CMakeLists.txt` tells CMake:
- What C++ standard to use (C++17)
- What libraries to find (Crow, mysql-connector-cpp)
- Which source files to compile (main.cpp, db.cpp, routes.cpp)
- How to link them (mysqlcppconn)
- What to do after building (copy static/ to build/)

## What vcpkg Does

vcpkg is a C++ package manager (like npm for JavaScript or pip for Python).

`vcpkg.json` declares dependencies:
```json
{
  "dependencies": [
    "crow",
    {"name": "mysql-connector-cpp", "features": ["jdbc"]}
  ]
}
```

When CMake runs with the vcpkg toolchain file, vcpkg:
1. Downloads the Crow source code
2. Downloads mysql-connector-cpp source code
3. Compiles both libraries for your platform
4. Makes them findable by CMake's `find_package()`

This is why the build takes a while the first time — it's compiling
the entire Crow library and MySQL connector from source.
Subsequent builds are fast (cached).

## The jdbc Feature for mysql-connector-cpp

```json
{"name": "mysql-connector-cpp", "features": ["jdbc"]}
```

mysql-connector-cpp has two APIs:
- The new "X DevAPI" (document-store style, newer MySQL features)
- The classic "JDBC-style API" (PreparedStatement, ResultSet, Connection)

The `jdbc` feature enables the classic JDBC-style API that our code uses:
```cpp
#include <mysql/jdbc.h>
sql::Connection* conn = driver->connect(url, user, pass);
sql::PreparedStatement* stmt = conn->prepareStatement("SELECT ...");
```

Without the `jdbc` feature, these headers and classes wouldn't be available.

---

# SECTION 14 — Environment Variables : Why Configuration This Way

## What Environment Variables Are

Environment variables are key-value pairs set in the OS shell.
They are accessible to any program via `std::getenv()`.

```bash
export DB_HOST=prod-mysql.internal
export POOL_SIZE=20
./ticketing_engine
```

## Why Not Hardcode Values?

If you hardcoded `"127.0.0.1"` and `"root"` in the source:
- You'd have to recompile to change for production
- Your password would be in the source code (visible in git history)
- Different environments (dev/staging/prod) would need different binaries

With environment variables:
- One binary runs in all environments
- Secrets (DB_PASS) are not in source code
- Ops team can change config without a rebuild

## The Lambda That Reads Them

```cpp
auto get_env = [](const char* name, const char* default_val) -> std::string {
    const char* val = std::getenv(name);
    return val ? std::string(val) : std::string(default_val);
};
```

`std::getenv(name)` returns `nullptr` if the variable is not set.
The ternary `val ? ... : ...` returns the default if it's null.

This is a **lambda** (anonymous inline function) stored in the variable `get_env`.
It captures nothing `[]` — it only uses its parameters.
Called like a regular function: `get_env("DB_HOST", "127.0.0.1")`.

---

# SECTION 15 — The Frontend (index.html) : How It Fits In

## What It Is

A single HTML file with embedded JavaScript.
No framework (no React, no Vue).
Pure HTML + CSS + vanilla JS.

The backend serves it from disk when someone visits `GET /`.

## Auto-Polling

```javascript
fetchSeats();                         // run immediately on page load
setInterval(fetchSeats, 3000);        // run every 3 seconds
```

Every 3 seconds, JS calls `GET /seats?event_id=1`.
The response (JSON array of 50 seats) is used to re-render the grid.
Booked seats appear red, available seats appear green.

This is called **polling**. It's simple but not the most efficient approach.

A more advanced approach would be **WebSockets**:
- Server pushes seat updates to all connected browsers instantly
- No 3-second delay
- Less wasted requests

For this project, polling is sufficient and simpler.

## Optimistic UI

```javascript
// Before the server responds, show the seat as "booking..."
el.className = 'seat booking';
el.textContent = '···';

// After server responds:
if (res.ok) { showToast("Booked!") }
else if (res.status === 409) { showToast("Already taken!") }

// Always refresh from server after booking attempt
setTimeout(fetchSeats, 200);
```

The UI immediately shows a loading state (orange, spinning).
If the booking fails (409), the seat goes back to its real state
after the next `fetchSeats()` call. This is called optimistic UI —
assume success, correct on failure.

---

# SECTION 16 — What This Project Would Need for Production

This project is a learning/demonstration project. For real production:

**1. TLS/HTTPS**
All HTTP traffic is plaintext. In production, add TLS termination
(via Nginx or a load balancer in front of Crow).

**2. Authentication**
Anyone can book a seat. In production, users would authenticate
(JWT token, session cookie) and you'd verify they're allowed to book.

**3. Rate Limiting**
One user could send 10,000 booking requests per second.
Add rate limiting (per-IP request limits) to prevent abuse.

**4. Connection Pool Timeout**
`cv_.wait()` blocks forever if pool is empty.
Add `cv_.wait_for()` with a timeout to return 503 instead of hanging.

**5. Graceful Shutdown**
Ctrl+C immediately kills the server. In-flight requests lose their responses.
Add signal handling (SIGTERM) to wait for in-flight requests to complete.

**6. Logging**
Current logging uses `std::cout` and `std::cerr`.
In production, structured logging (JSON) to a log aggregation system.

**7. Metrics**
No metrics collection. In production, expose Prometheus metrics:
requests per second, booking success rate, pool wait time, etc.

**8. Multiple Instances**
For real scale, run multiple C++ instances behind a load balancer.
The SELECT FOR UPDATE approach works correctly because the lock is
at the MySQL level — all instances share the same MySQL, same locks.

**9. MySQL Replication**
Single MySQL instance is a single point of failure.
In production: MySQL primary + read replicas + automatic failover.

**10. Connection Pool wait_timeout Handling**
MySQL closes idle connections after 8 hours.
Current code checks `isClosed()` on acquire, but `isClosed()` may not
always detect a server-side close. A more robust approach: send a
`SELECT 1` ping before returning the connection to verify it's alive.
