# How This Project Works — Explained From Zero

> Read this top to bottom. Each section builds on the previous one.
> No prior knowledge assumed.

---

# CHAPTER 1 — What Problem Are We Solving?

Imagine a movie booking website. 500 people are trying to book the last seat at the same time.

The question is: **what stops two people from booking the same seat?**

If you don't handle this carefully, both people get a "booking confirmed" email,
but there's only one seat. That's a double-booking bug.

**This project is a backend server that solves exactly this problem.**

It is written in C++ and uses MySQL as the database.

---

# CHAPTER 2 — What Is a Backend Server?

Before we look at any code, understand the pieces involved.

```
  [ Browser ]  ←→  [ Backend Server ]  ←→  [ MySQL Database ]
```

**Browser** — the web page the user sees. Written in HTML + JavaScript.
            It sends requests to the backend and shows what comes back.

**Backend Server** — our C++ program. It receives requests from browsers,
                   talks to the database, and sends back responses.
                   This is the code we wrote.

**MySQL Database** — stores all the data permanently.
                   Even if the server restarts, the data is still there.
                   Tables: `events` (the show) and `seats` (50 seats).

---

# CHAPTER 3 — What Is HTTP? (How Browser Talks to Server)

HTTP is a simple language that browsers and servers use to talk to each other.

A **request** looks like this:
```
POST /book HTTP/1.1
Content-Type: application/json

{"event_id": 1, "seat_id": 7, "user": "alice"}
```
- `POST` = the action (sending data)
- `/book` = the URL path (which feature to use)
- The bottom part = the data being sent (JSON format)

A **response** looks like this:
```
HTTP/1.1 200 OK
Content-Type: application/json

{"success": true, "message": "Seat booked!"}
```
- `200` = a status code meaning "everything worked"
- The bottom part = the data sent back

**Status codes you will see in this project:**
```
200 → OK, worked perfectly
400 → Bad Request (you sent wrong/missing data)
404 → Not Found (that seat ID doesn't exist)
409 → Conflict (seat is already taken)
500 → Internal Server Error (something crashed on the server)
503 → Service Unavailable (server is overloaded)
```

---

# CHAPTER 4 — The Files in This Project and What They Do

```
backend_projet/
│
├── src/
│   ├── main.cpp     ← The starting point. Like the "main entrance" of the program.
│   │                   It reads settings, connects to MySQL, and starts the server.
│   │
│   ├── db.h         ← Describes what the database helper looks like (the blueprint).
│   ├── db.cpp       ← The actual database helper code.
│   │                   Manages a "pool" of MySQL connections (explained in Chapter 6).
│   │
│   ├── routes.h     ← Declares the function that registers all URL handlers.
│   └── routes.cpp   ← The heart of the app. Contains all the "what to do when
│                       someone visits /seats or /book" logic.
│
├── sql/
│   └── schema.sql   ← The SQL that creates the tables and inserts starting data.
│
├── static/
│   └── index.html   ← The web page the user sees. Seat grid, buttons, etc.
│
├── docker-compose.yml ← Starts a MySQL database inside Docker (like a mini virtual machine).
├── CMakeLists.txt     ← Tells the compiler how to build the project.
└── vcpkg.json         ← Lists the libraries we need (Crow, MySQL driver).
```

---

# CHAPTER 5 — What Happens When You Start the Server

Let's follow the program from the very beginning.

---

### Step 1 — You run the program

```
./build/ticketing_engine
```

This launches `main.cpp`.

---

### Step 2 — It reads configuration

```cpp
// From main.cpp
const std::string db_host = get_env("DB_HOST", "127.0.0.1");
const int         db_port = std::stoi(get_env("DB_PORT", "3306"));
const std::string db_user = get_env("DB_USER", "root");
const std::string db_pass = get_env("DB_PASS", "root");
const std::string db_name = get_env("DB_NAME", "ticketing");
const int         pool_sz = std::stoi(get_env("POOL_SIZE", "10"));
const int         port    = std::stoi(get_env("PORT", "18080"));
const int         threads = std::stoi(get_env("THREADS", "4"));
```

`get_env` reads from environment variables. If none are set, it uses the defaults shown above.

This means you can run it like this without changing any code:
```
DB_HOST=myserver.com PORT=9000 ./ticketing_engine
```

---

### Step 3 — It connects to MySQL (creates the pool)

```cpp
Db db(db_host, db_port, db_user, db_pass, db_name, pool_sz);
```

This one line creates 10 MySQL connections and stores them ready for use.

Why 10? Because up to 10 requests might come in at the same time.
Each request needs its own connection. We prepare them all upfront
so we don't waste time creating them when a request arrives.

This is called a **connection pool**. (Explained fully in Chapter 6.)

---

### Step 4 — It registers all the URL routes

```cpp
crow::SimpleApp app;
register_routes(app, db);
```

This tells Crow: "when someone visits /seats, run THIS function.
When someone visits /book, run THAT function."

Think of it like a receptionist: "if you want seats info, go to desk A.
If you want to book, go to desk B."

---

### Step 5 — It starts listening for requests

```cpp
app.port(18080).concurrency(4).run();
```

- `.port(18080)` → listen on port 18080
- `.concurrency(4)` → create 4 threads (4 workers that can each handle one request at a time)
- `.run()` → start the server. The program now waits here forever.

The terminal shows:
```
[Server] Starting on http://0.0.0.0:18080
```

Server is live. It will now handle requests until you press Ctrl+C.

---

# CHAPTER 6 — What Is a Connection Pool? (Very Important)

This is one of the most important concepts in the project.

### The Problem Without a Pool

Every time a request comes in, if you create a fresh MySQL connection:

```
Request arrives → connect to MySQL (takes ~10ms) → run query → disconnect
```

10 milliseconds doesn't sound like much, but:
- 1000 requests per second × 10ms = your server is spending all its time just connecting
- Connecting means: TCP handshake + MySQL login + select database — expensive!

### The Solution: Pool

Create all connections ONCE at startup. Reuse them.

```
Startup: create 10 connections → store in a queue

Request arrives → grab a connection from the queue (takes ~0ms) → run query → put it back
```

### How It Looks in Memory

```
The pool is a std::queue (a line of connections):

      FRONT                          BACK
        │                              │
        ▼                              ▼
  [ conn1 ][ conn2 ][ conn3 ] ... [ conn10 ]

All 10 are available and ready.
```

When a request comes in:
```
BEFORE:   [ conn1 ][ conn2 ][ conn3 ] ... [ conn10 ]   (10 available)
          conn1 gets popped out ↓
DURING:   [ conn2 ][ conn3 ][ conn4 ] ... [ conn10 ]   (9 available)
          conn1 is being used by this request
AFTER:    [ conn2 ][ conn3 ][ conn4 ] ... [ conn10 ][ conn1 ]   (10 available again)
          conn1 was pushed back after the request finished
```

### What If All 10 Are Busy?

```
11th request arrives → tries to grab a connection → queue is empty
→ the thread SLEEPS  (cv_.wait)
→ uses ZERO CPU while sleeping — it's just waiting
→ as soon as any of the 10 requests finishes and returns its connection
→ cv_.notify_one() wakes up the sleeping thread
→ it grabs the returned connection and proceeds
```

### The Code That Does This (db.cpp)

```cpp
// ACQUIRE: grab a connection
PooledConnection Db::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);

    // If queue is empty, sleep and wait
    cv_.wait(lock, [this] { return !pool_.empty(); });

    // Pop from front of queue
    sql::Connection* conn = pool_.front();
    pool_.pop();

    // Check if MySQL dropped the connection while it was idle
    if (conn->isClosed()) {
        delete conn;
        conn = create_connection(); // make a fresh one
    }

    // Return a PooledConnection wrapper
    return PooledConnection(conn, [this](sql::Connection* c) { this->release(c); });
    //                                  ↑
    //              This lambda (anonymous function) will be called
    //              automatically when the PooledConnection is destroyed
}

// RELEASE: return a connection
void Db::release(sql::Connection* conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    pool_.push(conn);       // push back to queue
    cv_.notify_one();       // wake up one sleeping thread
}
```

### What Is PooledConnection?

It is a wrapper object. When you call `db.acquire()` you get a `PooledConnection`.

The magic: **when the PooledConnection object is destroyed (goes out of scope),
its destructor automatically calls `release()`**.

This is called **RAII** — you never have to manually return the connection.
The language handles it for you, even if an exception crashes the handler.

```cpp
{
    auto conn = db.acquire();   // ← connection checked out
    conn->prepareStatement(...);
    // ... do stuff ...
}   // ← conn goes out of scope HERE
    //   destructor fires
    //   connection returned to pool automatically ✓
```

---

# CHAPTER 7 — What Is a Thread?

`concurrency(4)` creates 4 threads. What is a thread?

Your computer can do multiple things at once. A **thread** is one worker
that can execute code. If you have 4 threads, 4 requests can be handled
at the same time.

```
Request 1 → Thread 1 handles it
Request 2 → Thread 2 handles it
Request 3 → Thread 3 handles it
Request 4 → Thread 4 handles it
Request 5 → waits for one of the threads to finish
```

But threads sharing the same data create problems.
Two threads could try to pop from the same queue at the same time.
That's why we have a **mutex**.

### What Is a Mutex?

Mutex = Mutual Exclusion = "only one thread at a time can enter here"

```cpp
std::lock_guard<std::mutex> lock(mutex_);
// Only ONE thread can be here at a time
// Other threads trying to enter will WAIT at this line
pool_.push(conn);
cv_.notify_one();
// When this scope ends, the mutex is released → next thread can enter
```

Without the mutex, two threads could both pop the same connection
from the queue — now two handlers are using the same MySQL connection
at the same time → corrupted data.

---

# CHAPTER 8 — What Happens When the Browser Visits GET /

```
Browser types: http://localhost:18080

STEP 1: Browser sends  GET /  to the server

STEP 2: Crow receives it, picks Thread 1 (whichever is free)

STEP 3: The route handler for "/" runs:
        - Opens the file  static/index.html  from disk
        - Reads all the HTML content into a string

STEP 4: Sends it back to the browser
        HTTP 200 OK
        Content-Type: text/html
        [HTML content of index.html]

STEP 5: Browser renders the HTML → user sees the seat grid
        50 seats shown as green squares

STEP 6: JavaScript at the bottom of index.html runs:
        fetchSeats()                     ← load seat data immediately
        setInterval(fetchSeats, 3000)    ← reload every 3 seconds
```

The page automatically stays fresh because it keeps asking
the server "what's the current seat status?" every 3 seconds.

---

# CHAPTER 9 — What Happens When JS Calls GET /seats

```
STEP 1: JavaScript runs:
        fetch("/seats?event_id=1")

STEP 2: Server receives  GET /seats?event_id=1

STEP 3: Route handler reads the query parameter
        event_id_str = req.url_params.get("event_id")  → "1"
        event_id = 1

STEP 4: db.acquire() — get a MySQL connection from the pool

STEP 5: Create a PreparedStatement (a safe SQL query)

        The SQL:
        ┌────────────────────────────────────────────────────────┐
        │  SELECT id, seat_label, is_booked,                     │
        │         COALESCE(booked_by, '') AS booked_by           │
        │  FROM seats                                            │
        │  WHERE event_id = ?                                    │
        │  ORDER BY seat_label                                   │
        └────────────────────────────────────────────────────────┘

        The ? is a placeholder. We fill it safely:
        stmt->setInt(1, event_id);  → replaces ? with 1

        Why not just write "WHERE event_id = 1" directly?
        → Because if a user sends malicious input, prepared statements
          protect us. More on this in Chapter 13.

STEP 6: MySQL runs the query, returns 50 rows

STEP 7: We loop through all rows and build a JSON string:
        result = [
          {"id":1, "seat_label":"A1", "is_booked":false, "booked_by":""},
          {"id":2, "seat_label":"A2", "is_booked":true,  "booked_by":"alice"},
          ...50 rows total...
        ]

        NOTE: COALESCE(booked_by, '') means:
        "if booked_by is NULL (empty in DB), return '' instead"
        So we never send null to the browser, always a string.

STEP 8: PooledConnection goes out of scope → connection returned to pool

STEP 9: Send response:
        HTTP 200 OK
        Content-Type: application/json
        [the JSON array]

STEP 10: JavaScript receives the JSON, re-draws the seat grid
         Green = is_booked: false
         Red   = is_booked: true
```

---

# CHAPTER 10 — What Happens When a User Books a Seat (POST /book)

This is the most complex and most important flow. Read carefully.

```
STEP 1: User clicks a green seat (e.g. seat B4)

STEP 2: JavaScript sends:
        POST /book
        Body: {"event_id": 1, "seat_id": 7, "user": "alice"}

STEP 3: Server receives the request, picks a free thread

STEP 4: Parse and validate the JSON body

        // Parse JSON
        auto body = crow::json::load(req.body);
        if (!body)  → return 400  "Invalid JSON body"

        // Extract fields
        event_id = body["event_id"].i()   → 1
        seat_id  = body["seat_id"].i()    → 7
        user     = body["user"].s()       → "alice"

        // Validate
        if any field missing  → return 400
        if user is empty      → return 400

STEP 5: db.acquire() — get a connection from the pool

STEP 6: BEGIN TRANSACTION
        conn->setAutoCommit(false);

        Normally MySQL commits (saves) every statement instantly.
        setAutoCommit(false) turns that off.
        Now nothing is saved until we explicitly call commit().
        If something goes wrong, we call rollback() to undo everything.

STEP 7: SELECT FOR UPDATE — THE KEY STEP

        SQL:
        ┌────────────────────────────────────────────────────────┐
        │  SELECT id, is_booked, seat_label                      │
        │  FROM seats                                            │
        │  WHERE id = 7 AND event_id = 1                        │
        │  FOR UPDATE                                            │
        └────────────────────────────────────────────────────────┘

        What "FOR UPDATE" does:
        MySQL InnoDB puts an EXCLUSIVE LOCK on this specific row.
        Think of it like putting a sticky note on the row that says
        "I am working on this row, nobody else touch it."

        Any other transaction that tries to SELECT FOR UPDATE the same row
        will PAUSE and wait until we finish (COMMIT or ROLLBACK).

        This is how we prevent double booking.

STEP 8: Check what the query returned

        Did we get a row back?
        → NO  → conn->rollback() → return 404 "Seat not found"

        Is is_booked = 1?
        → YES → conn->rollback() → return 409 "Seat already booked"

        Is is_booked = 0?
        → YES → continue to the next step ✓

STEP 9: UPDATE the seat

        SQL:
        ┌────────────────────────────────────────────────────────┐
        │  UPDATE seats                                          │
        │  SET is_booked = 1,                                    │
        │      booked_by = 'alice',                              │
        │      booked_at = NOW()                                 │
        │  WHERE id = 7 AND event_id = 1                        │
        └────────────────────────────────────────────────────────┘

STEP 10: COMMIT
         conn->commit();
         → Changes are NOW permanently saved to the database
         → The row lock on seat 7 is RELEASED
         → Any other transaction that was waiting for this row
           can now proceed

STEP 11: conn->setAutoCommit(true)  ← restore normal mode

STEP 12: PooledConnection goes out of scope → back to pool

STEP 13: Send response:
         HTTP 200 OK
         {"success":true, "message":"Seat booked successfully", "seat_label":"B4", "user":"alice"}

STEP 14: Browser receives 200
         Shows green toast: "Seat B4 booked for alice! ✓"
         Calls fetchSeats() after 200ms to refresh the grid
         Seat B4 turns red
```

---

# CHAPTER 11 — The Double Booking Problem, Explained Visually

This is the hardest concept. Let's go very slowly.

### Scene: Alice and Bob click the same seat at the same millisecond

---

### Without protection (WRONG — leads to double booking):

```
TIME →  0ms          1ms          2ms          3ms

Alice:  reads B4     B4 is free!  writes B4=booked
        is_booked=0              (saves to DB)
                                               returns 200 ✓

Bob:    reads B4     B4 is free!               writes B4=booked
        is_booked=0              (also saves!)
                                               returns 200 ✓ ← WRONG!

Both got 200. Same seat. Double booking. 💥
```

The problem: Alice read is_booked=0, then Bob read is_booked=0 (before Alice saved),
then both wrote is_booked=1. MySQL accepted both writes because each one was valid
at the time it was executed.

---

### With our protection (CORRECT — using SELECT FOR UPDATE):

```
TIME →  0ms              1ms              2ms              3ms

Alice:  SELECT B4        MySQL LOCKS row  UPDATE B4=booked COMMIT
        FOR UPDATE  →    for Alice        (saves to DB)    Lock released
        gets lock ✓

Bob:    SELECT B4
        FOR UPDATE  →    ⏸ BLOCKED        ⏸ BLOCKED        Bob unblocks
        waiting...       waiting...        waiting...       reads B4
                                                            is_booked = 1
                                                            ROLLBACK
                                                            returns 409 ✗

Alice returns 200 ✓ "Seat booked!"
Bob   returns 409 ✗ "Seat already booked"

One booking. Zero duplicates. ✓
```

The key: "FOR UPDATE" means "lock this row exclusively for me until I commit."
Bob's SELECT cannot even READ the row until Alice finishes.
By the time Bob reads it, Alice has already written is_booked=1.
Bob sees it, rolls back, and gets the correct 409 error.

---

# CHAPTER 12 — What Is a Transaction? (ACID)

A **transaction** is a group of SQL statements that all succeed together
or all fail together. There is no "half done."

In our booking:
```
BEGIN TRANSACTION
  SELECT B4 FOR UPDATE   ← step 1
  check is_booked        ← step 2
  UPDATE B4 = booked     ← step 3
COMMIT (or ROLLBACK)
```

If the server crashes after the UPDATE but before the COMMIT:
→ MySQL's redo log kicks in
→ On restart, MySQL sees the incomplete transaction
→ It automatically ROLLS BACK (undoes the UPDATE)
→ The seat stays as is_booked=0 — clean state

This is called **Atomicity** — all or nothing.

**ACID** is an acronym for 4 properties every transaction guarantees:

```
A = Atomicity   → all steps succeed, or none do
C = Consistency → database stays valid (UNIQUE constraints, foreign keys)
I = Isolation   → two transactions don't interfere with each other
D = Durability  → once committed, data survives crashes (written to disk)
```

---

# CHAPTER 13 — What Is SQL Injection and Why Don't We Have It?

SQL injection is a famous hacking technique.

### The Attack (if we wrote bad code):

```cpp
// BAD — never do this
string sql = "SELECT * FROM seats WHERE id = " + user_input;
```

If a hacker sends `user_input = "1 OR 1=1"`:
```sql
SELECT * FROM seats WHERE id = 1 OR 1=1
```
`1=1` is always true → returns ALL rows → hacker sees everything.

If a hacker sends `user_input = "1; DROP TABLE seats; --"`:
```sql
SELECT * FROM seats WHERE id = 1; DROP TABLE seats; --
```
Table deleted. Your entire data is gone.

### Our Defense (prepared statements):

```cpp
// GOOD — what this project does
stmt = conn->prepareStatement("SELECT * FROM seats WHERE id = ?");
stmt->setInt(1, seat_id);
```

MySQL compiles the SQL structure first. Then we send the value separately.
The value is NEVER treated as SQL code — it's always just data.
"1 OR 1=1" would be stored literally as the string "1 OR 1=1", not executed.

Every single query in this project uses prepared statements. Injection is impossible.

---

# CHAPTER 14 — What Is RAII? (Why Nothing Ever Leaks)

RAII = Resource Acquisition Is Initialization.

It sounds scary. It just means:
**"when an object is created it grabs a resource, when it's destroyed it releases it."**

The C++ language guarantees destructors ALWAYS run when an object goes out of scope —
even if there was an exception.

### In This Project:

**PooledConnection (db.h)**
```
Created  → MySQL connection popped from pool
Destroyed → MySQL connection pushed back to pool automatically
```

**std::unique_ptr<sql::PreparedStatement>**
```
Created  → holds a MySQL PreparedStatement object
Destroyed → calls delete on the PreparedStatement automatically
```

**std::lock_guard<std::mutex>**
```
Created  → locks the mutex (no other thread can enter)
Destroyed → unlocks the mutex automatically
```

### Why This Matters:

Without RAII, you'd have to write:
```cpp
// Without RAII — dangerous
auto conn = db.acquire();
auto stmt = conn->prepareStatement(...);
stmt->executeQuery();
// ... if an exception happens here, we never reach the cleanup below!
stmt->close();    // might never run
db.release(conn); // might never run → connection leaked forever!
```

With RAII, destructions happen automatically:
```cpp
// With RAII — safe
auto conn = db.acquire();
{
    std::unique_ptr<sql::PreparedStatement> stmt(conn->prepareStatement(...));
    stmt->executeQuery();
    // exception thrown here!
} // unique_ptr destructor runs → stmt freed ✓
  // conn destructor runs → connection back to pool ✓
  // no leaks, ever
```

---

# CHAPTER 15 — What Does the Database Schema Look Like?

These are the two tables stored in MySQL:

### Table: events
```
┌────┬───────────────────────────────┬──────────────────┬─────────────────────┐
│ id │ name                          │ venue            │ event_date          │
├────┼───────────────────────────────┼──────────────────┼─────────────────────┤
│  1 │ Grand Tech Conference 2026    │ Convention Center│ 2026-09-15 18:00:00 │
└────┴───────────────────────────────┴──────────────────┴─────────────────────┘
```

### Table: seats (50 rows, A1 through E10)
```
┌────┬──────────┬────────────┬───────────┬───────────┬─────────────────────┐
│ id │ event_id │ seat_label │ is_booked │ booked_by │ booked_at           │
├────┼──────────┼────────────┼───────────┼───────────┼─────────────────────┤
│  1 │        1 │ A1         │         0 │ NULL      │ NULL                │
│  2 │        1 │ A2         │         1 │ alice     │ 2026-08-16 10:23:00 │
│  3 │        1 │ A3         │         0 │ NULL      │ NULL                │
│ .. │       .. │ ..         │        .. │ ..        │ ..                  │
│ 50 │        1 │ E10        │         0 │ NULL      │ NULL                │
└────┴──────────┴────────────┴───────────┴───────────┴─────────────────────┘
```

Important column rules:
```
is_booked = 0 → seat is available (green in the browser)
is_booked = 1 → seat is taken    (red in the browser)

UNIQUE KEY on (event_id, seat_label):
  → You cannot have two rows with same event AND same seat
  → Database enforces this even if the C++ code had a bug

FOREIGN KEY: event_id → events.id
  → You cannot insert a seat for event_id=99 if event 99 doesn't exist
  → If you delete event 1, all its seats are automatically deleted too (CASCADE)

ENGINE=InnoDB:
  → Required for transactions (BEGIN/COMMIT/ROLLBACK)
  → Required for SELECT FOR UPDATE (row-level locking)
  → MyISAM (the alternative) only supports table-level locks — not good enough
```

---

# CHAPTER 16 — How Docker Sets Up MySQL

You don't need to install MySQL on your machine. Docker runs it in an isolated container.

```
STEP 1: You run    docker compose up -d

STEP 2: Docker downloads the mysql:8.0 image (if not already there)
        An "image" is like a pre-packaged virtual machine snapshot

STEP 3: Docker starts the MySQL container with these settings:
        MYSQL_ROOT_PASSWORD = root
        MYSQL_DATABASE      = ticketing
        Port 3306 on your machine → port 3306 inside the container

STEP 4: MySQL container starts and automatically runs:
        sql/schema.sql  (mounted into  /docker-entrypoint-initdb.d/)

        This creates both tables and inserts the 50 seed seats.

STEP 5: MySQL is now running at   localhost:3306
        Your C++ server connects to it as if it were a normal MySQL install

STEP 6: When you stop Docker:    docker compose down
        The data is preserved in a Docker volume (mysqldata)
        Next time you start it, your bookings are still there
```

---

# CHAPTER 17 — How to Build and Run

```
STEP 1: Start MySQL
        docker compose up -d

STEP 2: Set VCPKG_ROOT (vcpkg is the C++ package manager)
        export VCPKG_ROOT=/path/to/your/vcpkg

STEP 3: Build
        cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        cmake --build build -j4

        What this does:
        - vcpkg downloads and builds: crow + mysql-connector-cpp
        - CMake compiles: main.cpp + db.cpp + routes.cpp
        - Links everything into one binary: build/ticketing_engine

STEP 4: Run
        ./build/ticketing_engine

STEP 5: Open browser
        http://localhost:18080
        You see the seat grid!
```

Or use the one-command setup:
```
export VCPKG_ROOT=/path/to/vcpkg
chmod +x scripts/setup.sh
./scripts/setup.sh
./build/ticketing_engine
```

---

# CHAPTER 18 — The Full Picture in One Simple View

```
YOU open http://localhost:18080
         │
         │  GET /
         ▼
┌────────────────────────────────────────────────────────┐
│                C++ Backend (ticketing_engine)           │
│                                                        │
│  main.cpp                                              │
│    ├── Created 10 MySQL connections (pool)             │
│    └── Started Crow with 4 threads                     │
│                                                        │
│  Thread 1 ──► GET /  ──────────────────► sends HTML   │
│  Thread 2 ──► GET /seats ──► MySQL query ► sends JSON │
│  Thread 3 ──► POST /book                               │
│  │             ├── parse JSON                          │
│  │             ├── acquire connection from pool        │
│  │             ├── BEGIN TRANSACTION                   │
│  │             ├── SELECT seat FOR UPDATE  ──► MySQL   │
│  │             │     └── row is LOCKED                 │
│  │             ├── check is_booked                     │
│  │             ├── UPDATE seat ──────────► MySQL       │
│  │             ├── COMMIT  ──────────────► MySQL       │
│  │             │     └── row lock RELEASED             │
│  │             └── return 200 ─────────────────────────────► YOU
│  Thread 4 ──► waiting for next request                │
│                                                        │
│  Connection Pool:                                      │
│  [ conn1 ][ conn2 ][ conn3 ] ... [ conn10 ]           │
│  Connections auto-return when handler finishes (RAII)  │
└────────────────────────────────────────────────────────┘
         │
         │  TCP :3306
         ▼
┌────────────────────────────────┐
│  MySQL 8.0 (Docker)            │
│  database: ticketing           │
│  table: events (1 row)         │
│  table: seats  (50 rows)       │
│                                │
│  InnoDB engine:                │
│  - Row-level locking           │
│  - ACID transactions           │
│  - SELECT FOR UPDATE           │
└────────────────────────────────┘
```

---

# Quick Glossary

| Word | Simple Meaning |
|------|----------------|
| **Backend** | The server-side program. Users don't see it directly. |
| **HTTP** | The language browsers and servers use to talk. |
| **GET** | HTTP request to read/fetch data. |
| **POST** | HTTP request to send data and trigger an action. |
| **JSON** | A simple text format for sending structured data. `{"key":"value"}` |
| **MySQL** | A database that stores data in tables permanently. |
| **InnoDB** | MySQL's engine that supports transactions and row locks. |
| **Transaction** | A group of SQL steps that all succeed or all fail together. |
| **SELECT FOR UPDATE** | A SQL statement that reads a row AND locks it so nobody else can change it. |
| **Connection Pool** | Pre-created database connections reused across requests. |
| **Thread** | An independent worker that can handle one request at a time. |
| **Mutex** | A lock that lets only one thread access shared data at a time. |
| **RAII** | C++ pattern where objects auto-clean up resources in their destructor. |
| **PreparedStatement** | SQL with safe placeholders `?` instead of pasted-in values. |
| **SQL Injection** | A hacking attack where malicious SQL is embedded in user input. |
| **ACID** | 4 guarantees of a database transaction: Atomic, Consistent, Isolated, Durable. |
| **Race Condition** | A bug where two threads access shared data at the same time and corrupt it. |
| **Docker** | A tool that runs programs in isolated containers. We use it to run MySQL. |
| **CMake** | A tool that describes how to compile a C++ project. |
| **vcpkg** | A C++ package manager. Downloads and builds libraries like Crow. |
| **Crow** | A C++ web framework. Handles HTTP requests, routing, and JSON. |
| **CORS** | A browser rule. Our server adds headers to allow cross-origin requests. |
| **Lambda** | A small anonymous function written inline. `[&db]() { ... }` |
| **unique_ptr** | A C++ smart pointer that automatically deletes what it holds. |
| **ROLLBACK** | Undo all SQL changes in the current transaction. |
| **COMMIT** | Save all SQL changes in the current transaction permanently. |
