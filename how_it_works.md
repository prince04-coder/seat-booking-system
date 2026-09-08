# How It Works — Complete Execution Flow

> This file shows WHAT ACTUALLY HAPPENS when the code runs.
> Every function call, every line of code, every step traced from start to finish.
> Read this alongside the source files.

---

# PART 1 — From Binary Launch to Server Ready

## What happens the moment you run `./build/ticketing_engine`

The OS loads the binary into memory and calls `main()` in `src/main.cpp`.

---

### Step 1 — The banner prints

```cpp
std::cout << R"(
 ╔══════════════════════════════════════════════════════╗
 ║       High-Concurrency Ticketing Engine              ║
 ║       C++ · MySQL · Crow                             ║
 ╚══════════════════════════════════════════════════════╝
)" << std::endl;
```

This is just cosmetic output. `R"(...)"` is a raw string literal —
no escape sequences needed inside it, newlines are literal.

---

### Step 2 — Environment variables are read

```cpp
auto get_env = [](const char* name, const char* default_val) -> std::string {
    const char* val = std::getenv(name);
    return val ? std::string(val) : std::string(default_val);
};

const std::string db_host = get_env("DB_HOST", "127.0.0.1");
const int         db_port = std::stoi(get_env("DB_PORT", "3306"));
const std::string db_user = get_env("DB_USER", "root");
const std::string db_pass = get_env("DB_PASS", "root");
const std::string db_name = get_env("DB_NAME", "ticketing");
const int         pool_sz = std::stoi(get_env("POOL_SIZE", "10"));
const int         port    = std::stoi(get_env("PORT", "18080"));
const int         threads = std::stoi(get_env("THREADS", "4"));
```

`get_env` is a lambda (anonymous function). It calls the C standard library
function `std::getenv()` which reads from the OS environment.

If you ran: `DB_HOST=192.168.1.5 ./ticketing_engine`
then `std::getenv("DB_HOST")` returns `"192.168.1.5"` (a char*).
If the variable is not set, `getenv` returns `nullptr`,
and the ternary `val ? val : default_val` uses the default instead.

`std::stoi()` converts a string like `"3306"` to the integer `3306`.

---

### Step 3 — The Db object is constructed (connection pool created)

```cpp
Db db(db_host, db_port, db_user, db_pass, db_name, pool_sz);
```

This calls `Db::Db(...)` in `src/db.cpp`. Here is exactly what happens:

```cpp
Db::Db(const std::string& host, int port,
       const std::string& user, const std::string& password,
       const std::string& database, size_t pool_size)
    : user_(user), password_(password), database_(database), pool_size_(pool_size)
{
    // Build the connection URL string
    url_ = "tcp://" + host + ":" + std::to_string(port);
    // url_ is now "tcp://127.0.0.1:3306"

    // Get the MySQL driver singleton
    // This is a global object managed by the MySQL connector library
    // You must NOT delete it
    driver_ = sql::mysql::get_mysql_driver_instance();

    // Create pool_size connections in a loop
    for (size_t i = 0; i < pool_size_; ++i) {
        pool_.push(create_connection());
        // prints: [Db] Connection 1/10 ready.
        //         [Db] Connection 2/10 ready.
        //         ... up to 10
    }
    // prints: [Db] Pool ready — 10 connections.
}
```

Each call to `create_connection()` does:

```cpp
sql::Connection* Db::create_connection() {
    // TCP connect + MySQL handshake + authenticate
    sql::Connection* conn = driver_->connect(url_, user_, password_);

    // Send: USE ticketing;
    conn->setSchema(database_);

    return conn;
    // This raw pointer is now stored in pool_
}
```

After the constructor finishes:

```
pool_ queue contains:
[ conn1* ][ conn2* ][ conn3* ][ conn4* ][ conn5* ]
[ conn6* ][ conn7* ][ conn8* ][ conn9* ][ conn10* ]

All 10 are connected to MySQL and ready.
mutex_ is unlocked.
cv_ has no waiting threads.
```

---

### Step 4 — Routes are registered

```cpp
crow::SimpleApp app;
register_routes(app, db);
```

`register_routes()` in `src/routes.cpp` runs. It calls `CROW_ROUTE` for each URL.

`CROW_ROUTE(app, "/seats")` expands to a macro that:
1. Creates a Crow route object for the path "/seats"
2. Attaches a handler lambda to it
3. The lambda captures `db` by reference `[&db]`

After `register_routes()` returns, Crow's internal route table looks like:

```
"/"       → lambda (serves index.html)
"/seats"  → lambda (reads seats from MySQL)
"/book"   → lambda (books a seat with transaction)
"/reset"  → lambda (resets all seats)
"/health" → lambda (checks pool status)
```

No HTTP listening has started yet. These are just registered functions.

---

### Step 5 — Server starts listening

```cpp
app.port(18080).concurrency(4).run();
```

This is a chain of method calls:
- `.port(18080)` — sets the TCP port to listen on
- `.concurrency(4)` — creates 4 OS worker threads
- `.run()` — binds socket, starts listening, BLOCKS HERE FOREVER

Internally Crow:
1. Creates a TCP socket
2. Binds it to `0.0.0.0:18080` (all network interfaces)
3. Calls `listen()` on the socket
4. Spawns 4 threads using `std::thread` or Boost.Asio internally
5. Each thread runs an event loop waiting for connections

Terminal shows:
```
[Server] Starting on http://0.0.0.0:18080
```

The program is now blocked inside `.run()`.
It will never return unless you press Ctrl+C or the process is killed.

---

# PART 2 — What Happens When Browser Opens the Page

## Full trace: `GET /`

```
YOU TYPE: http://localhost:18080 in browser

BROWSER does:
  1. DNS lookup for "localhost" → 127.0.0.1
  2. TCP connect to 127.0.0.1:18080 (3-way handshake)
  3. Sends HTTP request:
     GET / HTTP/1.1
     Host: localhost:18080
     User-Agent: Mozilla/5.0 ...
     Accept: text/html,...

CROW does:
  1. Accepts the TCP connection on the main thread
  2. Reads the HTTP request bytes
  3. Parses: method=GET, path="/", headers=...
  4. Finds the matching route: "/"
  5. Picks a free worker thread (say Thread 1)
  6. Calls the "/" handler on Thread 1

HANDLER runs (src/routes.cpp):

  std::vector<std::string> paths = {
      "static/index.html",
      "../static/index.html",
      "../../static/index.html"
  };

  for (const auto& path : paths) {
      std::ifstream file(path);     // try to open the file
      if (file.is_open()) {
          std::ostringstream ss;
          ss << file.rdbuf();       // read entire file into string stream
          auto res = crow::response(200);
          res.set_header("Content-Type", "text/html; charset=utf-8");
          res.body = ss.str();      // set response body to HTML content
          return res;
      }
  }

  It tries "static/index.html" first.
  If found (it is, because cmake copied it to build/static/):
    → reads all HTML into a string
    → returns 200 response with the HTML

CROW sends response back:
  HTTP/1.1 200 OK
  Content-Type: text/html; charset=utf-8
  Content-Length: 14823

  <!DOCTYPE html>
  <html>...the full index.html content...

BROWSER receives HTML, renders the page:
  → Shows the seat grid (5 rows × 10 columns = 50 seats)
  → All seats start green (no data yet)

JAVASCRIPT in index.html runs immediately:
  fetchSeats();                        // called right away
  setInterval(fetchSeats, 3000);       // scheduled every 3 seconds
```

---

# PART 3 — What Happens on GET /seats

## Full trace: JavaScript calls `GET /seats?event_id=1`

```
JS sends: fetch("/seats?event_id=1")

HTTP request:
  GET /seats?event_id=1 HTTP/1.1
  Host: localhost:18080

CROW:
  1. Parses path="/seats", query="event_id=1"
  2. Matches route "/seats"
  3. Picks Thread 2 (or whichever is free)
  4. Calls the /seats handler lambda
```

### Inside the `/seats` handler:

```cpp
// Step 1: Read the query parameter
auto event_id_str = req.url_params.get("event_id");
// event_id_str = "1" (a C string pointer, or nullptr if not present)

int event_id = event_id_str ? std::stoi(event_id_str) : 1;
// event_id = 1

// Step 2: Acquire a connection from the pool
auto conn = db.acquire();
```

### Inside `db.acquire()`:

```cpp
PooledConnection Db::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    // mutex_ is now LOCKED by this thread
    // Other threads calling acquire() will BLOCK at this line

    cv_.wait(lock, [this] { return !pool_.empty(); });
    // pool_ has 10 connections → not empty → does NOT sleep
    // continues immediately

    sql::Connection* conn = pool_.front();  // conn1
    pool_.pop();
    // pool_ now has 9 connections: [ conn2 ]...[ conn10 ]

    // Check if MySQL dropped this connection
    // conn1 was just created at startup, it's fine
    if (conn->isClosed()) { ... }  // false, skipped

    // mutex_ is released here (unique_lock goes out of scope)
    return PooledConnection(conn1, [this](sql::Connection* c) {
        this->release(c);
    });
    // Returns a PooledConnection wrapping conn1
    // The lambda will be called when PooledConnection is destroyed
}
```

Back in the handler:

```cpp
// Step 3: Build and run the SQL query
std::unique_ptr<sql::PreparedStatement> stmt(
    conn->prepareStatement(
        "SELECT id, seat_label, is_booked, "
        "COALESCE(booked_by, '') AS booked_by "
        "FROM seats WHERE event_id = ? ORDER BY seat_label"
    )
);
// MySQL receives this SQL template and compiles it
// The ? is a placeholder

stmt->setInt(1, event_id);
// Binds the value 1 to the first ? placeholder
// MySQL now knows: WHERE event_id = 1

std::unique_ptr<sql::ResultSet> result(stmt->executeQuery());
// Sends the query to MySQL
// MySQL executes: SELECT id, seat_label, is_booked, COALESCE(booked_by,'')
//                 FROM seats WHERE event_id = 1 ORDER BY seat_label
// Returns 50 rows

// Step 4: Loop through results and build JSON
std::ostringstream oss;
oss << "[";
bool first = true;

while (result->next()) {
    // result->next() advances the cursor to the next row
    // Returns false when all rows have been read

    if (!first) oss << ",";
    first = false;

    // Read column values from current row
    int id            = result->getInt("id");           // e.g. 1
    std::string label = result->getString("seat_label"); // e.g. "A1"
    bool booked       = result->getBoolean("is_booked"); // e.g. false
    std::string by    = result->getString("booked_by");  // e.g. ""

    oss << "{"
        << "\"id\":"          << id              << ","
        << "\"seat_label\":\"" << label           << "\","
        << "\"is_booked\":"   << (booked ? "true" : "false") << ","
        << "\"booked_by\":\"" << by               << "\""
        << "}";
}
oss << "]";

// oss.str() is now:
// [{"id":1,"seat_label":"A1","is_booked":false,"booked_by":""},
//  {"id":2,"seat_label":"A10","is_booked":false,"booked_by":""},
//  ...50 items...]
```

```cpp
// Step 5: Return response
return json_response(200, oss.str());
// json_response() sets Content-Type: application/json
// sets Access-Control-Allow-Origin: *
// sets body to the JSON string
// returns crow::response(200)
```

```
// Step 6: PooledConnection goes out of scope
// conn's destructor fires:

PooledConnection::~PooledConnection() {
    if (conn_ && releaser_) {
        releaser_(conn_);  // calls: db.release(conn1)
    }
}

// db.release(conn1):
void Db::release(sql::Connection* conn) {
    std::lock_guard<std::mutex> lock(mutex_);  // lock
    pool_.push(conn);       // push conn1 back to queue
    cv_.notify_one();       // wake any sleeping thread
}
// pool_ now has 10 connections again: [ conn2 ]...[ conn10 ][ conn1 ]

CROW sends back:
  HTTP/1.1 200 OK
  Content-Type: application/json
  Access-Control-Allow-Origin: *

  [{"id":1,"seat_label":"A1","is_booked":false,"booked_by":""},...]

JAVASCRIPT receives the JSON:
  seats = await res.json();  // array of 50 objects
  renderGrid();              // redraws the seat grid
  updateStats();             // updates "Available: 50, Booked: 0"
```

---

# PART 4 — What Happens on POST /book (The Most Important Flow)

## Setup: User clicks seat B4 (seat_id = 14 in this example)

```
JS sends:
  POST /book HTTP/1.1
  Content-Type: application/json

  {"event_id": 1, "seat_id": 14, "user": "alice"}

CROW:
  Parses: method=POST, path="/book", body=the JSON string
  Picks Thread 3
  Calls the /book handler
```

### Inside the `/book` handler — step by step:

```cpp
// Step 1: Handle CORS preflight (not this case, it's POST not OPTIONS)
if (req.method == crow::HTTPMethod::Options) { ... }  // skipped

// Step 2: Parse the JSON body
auto body = crow::json::load(req.body);
// req.body = '{"event_id": 1, "seat_id": 14, "user": "alice"}'
// crow::json::load() parses this string
// If it's invalid JSON, body evaluates to false

if (!body) {
    return json_response(400, R"({"error":"Invalid JSON body"})");
}
// body is valid, continue

// Step 3: Extract fields
int event_id = static_cast<int>(body["event_id"].i());  // 1
int seat_id  = static_cast<int>(body["seat_id"].i());   // 14
std::string user = body["user"].s();                     // "alice"

// body["event_id"].i() returns a long long
// static_cast<int>() converts it to int

// Step 4: Validate
if (user.empty()) { ... }  // "alice" is not empty, continue

// Step 5: Acquire a connection
auto conn = db.acquire();
// Pops conn2 from pool (conn1 was returned earlier from GET /seats)
// Pool now has 9 connections

// Step 6: BEGIN TRANSACTION
conn->setAutoCommit(false);
// Tells MySQL: don't commit each statement automatically
// We are now in an explicit transaction
// Nothing will be permanently saved until we call commit()
```

### The critical section — SELECT FOR UPDATE:

```cpp
// Step 7: Lock the row with SELECT FOR UPDATE
std::unique_ptr<sql::PreparedStatement> sel(
    conn->prepareStatement(
        "SELECT id, is_booked, seat_label "
        "FROM seats WHERE id = ? AND event_id = ? "
        "FOR UPDATE"
    )
);
sel->setInt(1, seat_id);   // bind 14
sel->setInt(2, event_id);  // bind 1

std::unique_ptr<sql::ResultSet> result(sel->executeQuery());
```

At this exact moment in MySQL InnoDB:
```
MySQL executes:
  SELECT id, is_booked, seat_label
  FROM seats
  WHERE id = 14 AND event_id = 1
  FOR UPDATE

InnoDB:
  1. Locates row 14 in the seats table (using the primary key index)
  2. Checks for existing locks on this row — none
  3. Places an EXCLUSIVE (X) LOCK on row 14
  4. Returns the row data

The row is now LOCKED.
Any other transaction that tries:
  SELECT ... WHERE id = 14 FOR UPDATE
  UPDATE seats WHERE id = 14
  DELETE FROM seats WHERE id = 14
...will BLOCK and wait.
```

```cpp
// Step 8: Check if the row exists
if (!result->next()) {
    // No row returned — seat_id 14 doesn't exist for event 1
    conn->rollback();         // end transaction, release lock
    conn->setAutoCommit(true);
    return json_response(404, R"({"error":"Seat not found"})");
}
// Row exists, continue

// Step 9: Check if already booked
bool is_booked  = result->getBoolean("is_booked");  // false (0)
std::string seat_label = result->getString("seat_label");  // "B4"

if (is_booked) {
    conn->rollback();         // release the lock
    conn->setAutoCommit(true);
    return json_response(409,
        R"({"error":"Seat already booked","seat_label":")" + seat_label + R"("})");
}
// is_booked = false, seat is available, continue

// Step 10: UPDATE the seat
std::unique_ptr<sql::PreparedStatement> upd(
    conn->prepareStatement(
        "UPDATE seats SET is_booked = 1, booked_by = ?, booked_at = NOW() "
        "WHERE id = ? AND event_id = ?"
    )
);
upd->setString(1, user);      // bind "alice"
upd->setInt(2, seat_id);      // bind 14
upd->setInt(3, event_id);     // bind 1
upd->executeUpdate();

// MySQL executes:
//   UPDATE seats
//   SET is_booked = 1, booked_by = 'alice', booked_at = '2026-08-25 21:30:00'
//   WHERE id = 14 AND event_id = 1
//
// 1 row affected.
// The change is in the transaction buffer — NOT yet permanent.

// Step 11: COMMIT
conn->commit();
// MySQL:
//   1. Writes the change to the InnoDB redo log (fsync to disk)
//   2. Applies the change to the data page
//   3. RELEASES the exclusive lock on row 14
//   4. Sends "commit OK" to the C++ client
//
// The booking is now PERMANENT.
// Other transactions that were waiting for the lock on row 14 are UNBLOCKED.

conn->setAutoCommit(true);  // restore default mode

// Step 12: Log and respond
std::cout << "[/book] Seat B4 booked by alice" << std::endl;

return json_response(200,
    R"({"success":true,"message":"Seat booked successfully","seat_label":"B4","user":"alice"})");
```

```
// Step 13: PooledConnection destructs
// conn2 returned to pool
// cv_.notify_one() fires

CROW sends:
  HTTP/1.1 200 OK
  Content-Type: application/json
  Access-Control-Allow-Origin: *

  {"success":true,"message":"Seat booked successfully","seat_label":"B4","user":"alice"}

JAVASCRIPT receives 200:
  showToast("✓ Seat B4 booked for alice!", "success")
  setTimeout(fetchSeats, 200)  // refresh grid after 200ms
  // B4 turns red on the screen
```

---

# PART 5 — What Happens When Two Users Click the Same Seat Simultaneously

## The exact execution with two threads

```
Time 0ms: Alice (Thread 3) and Bob (Thread 4) both click seat B4
          Both send POST /book simultaneously

THREAD 3 (Alice):                    THREAD 4 (Bob):
  acquire conn2 from pool              acquire conn3 from pool
  setAutoCommit(false)                 setAutoCommit(false)

  SELECT seat 14 FOR UPDATE            SELECT seat 14 FOR UPDATE
  ↓                                    ↓
  MySQL receives SELECT FOR UPDATE     MySQL receives SELECT FOR UPDATE
  for row 14 from Thread 3 FIRST       for row 14 from Thread 4

  InnoDB: lock row 14 for Thread 3     InnoDB: row 14 is LOCKED by Thread 3
  Return: is_booked=0 ✓                Thread 4's query BLOCKS here
                                        ↕ waiting...
  is_booked = false → proceed           ↕ waiting...

  UPDATE seat 14 SET is_booked=1        ↕ waiting...

  COMMIT                                ↕ waiting...
  → row 14 lock RELEASED               ↕ Thread 4 UNBLOCKED

  return 200 "Seat booked!" ✓          InnoDB returns: is_booked=1 ✗

  conn2 returned to pool               is_booked = true → ROLLBACK

                                        return 409 "Seat already booked" ✗

                                        conn3 returned to pool
```

Alice gets the seat. Bob gets a clean 409 error. Zero double booking.

---

# PART 6 — What Happens on POST /reset

```
JS sends:
  POST /reset HTTP/1.1

HANDLER:
  auto conn = db.acquire();       // get any connection from pool
  std::unique_ptr<sql::Statement> stmt(conn->createStatement());
  // createStatement() (not prepareStatement) — for simple one-off SQL
  // no ? placeholders needed here

  stmt->executeUpdate(
      "UPDATE seats SET is_booked = 0, booked_by = NULL, booked_at = NULL"
  );
  // MySQL executes this against ALL 50 rows
  // No WHERE clause — affects every seat
  // This runs as a single autocommit statement (no explicit transaction)
  // autoCommit=true means MySQL commits immediately after this UPDATE

  std::cout << "[/reset] All seats reset." << std::endl;

  // conn returned to pool (RAII)
  return 200 {"success":true,"message":"All seats have been reset"}
```

---

# PART 7 — What Happens on GET /health

```
HANDLER:
  bool ok = db.available() > 0;
```

### Inside `db.available()`:

```cpp
size_t Db::available() const {
    std::lock_guard<std::mutex> lock(mutex_);  // lock
    return pool_.size();                        // count connections in queue
}
// mutex released when lock_guard destructs
```

```cpp
std::ostringstream oss;
oss << "{"
    << "\"status\":\""       << (ok ? "healthy" : "degraded") << "\","
    << "\"pool_available\":" << db.available()                 << ","
    << "\"pool_total\":"     << db.pool_size()
    << "}";

auto res = crow::response(ok ? 200 : 503);
res.set_header("Content-Type", "application/json");
res.body = oss.str();
return res;

// Response:
// {"status":"healthy","pool_available":10,"pool_total":10}
```

---

# PART 8 — The Connection Pool State at All Times

## Scenario: 3 simultaneous requests arrive

```
Initial state:
pool_: [ C1 ][ C2 ][ C3 ][ C4 ][ C5 ][ C6 ][ C7 ][ C8 ][ C9 ][ C10 ]
                                                               available = 10

Request A arrives (Thread 1):
  acquire() → pops C1
pool_: [ C2 ][ C3 ][ C4 ][ C5 ][ C6 ][ C7 ][ C8 ][ C9 ][ C10 ]
                                                               available = 9

Request B arrives (Thread 2):
  acquire() → pops C2
pool_: [ C3 ][ C4 ][ C5 ][ C6 ][ C7 ][ C8 ][ C9 ][ C10 ]
                                                               available = 8

Request C arrives (Thread 3):
  acquire() → pops C3
pool_: [ C4 ][ C5 ][ C6 ][ C7 ][ C8 ][ C9 ][ C10 ]
                                                               available = 7

Request A finishes:
  PooledConnection destructs → release(C1) → pool_.push(C1) → notify_one()
pool_: [ C4 ][ C5 ][ C6 ][ C7 ][ C8 ][ C9 ][ C10 ][ C1 ]
                                                               available = 8

Request B finishes → release(C2):
pool_: [ C4 ][ C5 ][ C6 ][ C7 ][ C8 ][ C9 ][ C10 ][ C1 ][ C2 ]
                                                               available = 9

Request C finishes → release(C3):
pool_: [ C4 ][ C5 ][ C6 ][ C7 ][ C8 ][ C9 ][ C10 ][ C1 ][ C2 ][ C3 ]
                                                               available = 10
```

## Scenario: 11th request arrives when all 10 are busy

```
pool_: []   ← empty, all 10 connections are in use
                                                               available = 0

Request 11 calls acquire():
  std::unique_lock lock(mutex_)      ← mutex locked
  cv_.wait(lock, predicate)          ← predicate: !pool_.empty()
                                        pool_ IS empty → predicate false
                                        cv_.wait() atomically:
                                          1. releases mutex_
                                          2. suspends Thread 11
                                          3. Thread 11 is OFF the CPU
                                             (zero CPU usage, just waiting)

Request 4 finishes:
  PooledConnection destructs
  release(C4) called:
    lock_guard lock(mutex_)          ← mutex locked
    pool_.push(C4)                   ← pool_ now has 1 item
    cv_.notify_one()                 ← wake ONE sleeping thread
    lock released

Thread 11 wakes up:
  cv_.wait() re-acquires mutex_
  checks predicate: !pool_.empty() → true → proceeds
  pops C4 from pool_
  releases mutex_
  continues with the request
```

---

# PART 9 — The RAII Chain: What Destructs in What Order

When a booking handler returns, here is the exact destruction order:

```cpp
// Inside the handler scope:
auto conn = db.acquire();                                          // [1]
conn->setAutoCommit(false);

std::unique_ptr<sql::PreparedStatement> sel(                       // [2]
    conn->prepareStatement("SELECT ... FOR UPDATE")
);
sel->setInt(1, seat_id);
std::unique_ptr<sql::ResultSet> result(sel->executeQuery());       // [3]

// ... check result, proceed ...

std::unique_ptr<sql::PreparedStatement> upd(                       // [4]
    conn->prepareStatement("UPDATE ...")
);
upd->setInt(...);
upd->executeUpdate();

conn->commit();
conn->setAutoCommit(true);

return json_response(200, ...);
// ↓ handler scope ends here
```

C++ destructs local variables in REVERSE ORDER of construction:

```
[4] upd unique_ptr destructs     → delete PreparedStatement (UPDATE)
[3] result unique_ptr destructs  → delete ResultSet
[2] sel unique_ptr destructs     → delete PreparedStatement (SELECT)
[1] conn PooledConnection destructs →
        calls releaser_(conn_)
        → db.release(conn2)
        → mutex locked
        → pool_.push(conn2)
        → cv_.notify_one()
        → mutex released
```

This all happens automatically. You write `return json_response(200, ...)` and
the language handles all the cleanup. You cannot skip it, forget it, or bypass it.

---

# PART 10 — What Happens During a MySQL Exception

Imagine MySQL crashes while a booking is being processed:

```cpp
auto conn = db.acquire();                                // conn popped from pool
conn->setAutoCommit(false);                              // transaction started

std::unique_ptr<sql::PreparedStatement> sel(...);
sel->setInt(1, seat_id);
std::unique_ptr<sql::ResultSet> result(sel->executeQuery()); // ← MYSQL CRASHES HERE
                                                              //   throws sql::SQLException
```

What C++ does automatically (stack unwinding):

```
Exception thrown: sql::SQLException("MySQL server has gone away")

Stack unwinds upward:

1. result's constructor threw → result was never created → nothing to destruct

2. sel unique_ptr destructs → delete PreparedStatement ✓

3. We hit the inner try/catch(...) block:
   catch (...) {
       try { conn->rollback(); } catch (...) {}   // attempt rollback (will also fail)
       try { conn->setAutoCommit(true); } catch (...) {}
       throw;  // re-throw the exception
   }

4. conn PooledConnection destructs:
   releaser_(conn_) → db.release(conn)
   → connection pushed back to pool
   (the connection is broken, but it's back in the pool)

5. Exception reaches outer catch:
   catch (const sql::SQLException& e) {
       std::cerr << "[/book] MySQL error: " << e.what() << std::endl;
       return json_response(500, R"({"error":"Database error"})");
   }

6. User receives:
   HTTP/1.1 500 Internal Server Error
   {"error":"Database error"}

7. Next request that uses the broken connection:
   acquire() pops it
   conn->isClosed() → true
   → delete the broken connection
   → create_connection() → fresh MySQL connection
   → pool is healthy again
```

---

# PART 11 — The Prepared Statement Lifecycle in Detail

Every SQL query goes through these exact steps:

```
STEP 1: conn->prepareStatement(sql_template)
        ↓
        C++ sends to MySQL over TCP:
          COM_STMT_PREPARE packet
          "SELECT id, is_booked, seat_label FROM seats WHERE id = ? AND event_id = ? FOR UPDATE"

        MySQL responds:
          Statement ID: 42 (or any number)
          Parameter count: 2 (two ? placeholders)
          Column count: 3 (id, is_booked, seat_label)

        The sql::PreparedStatement object stores this Statement ID.

STEP 2: stmt->setInt(1, seat_id) → setInt(2, event_id)
        ↓
        Stores the values locally in the PreparedStatement object.
        NOT sent to MySQL yet.

STEP 3: stmt->executeQuery() or stmt->executeUpdate()
        ↓
        C++ sends to MySQL over TCP:
          COM_STMT_EXECUTE packet
          Statement ID: 42
          Parameters: [14 (int), 1 (int)]  ← sent as binary, typed data

        MySQL:
          Looks up the compiled plan for statement 42
          Substitutes parameters into the plan
          Executes the query
          Returns result rows (or affected row count)

STEP 4: result->next() / result->getInt() / result->getString()
        ↓
        Crow reads rows from the ResultSet object
        which buffers the MySQL response data

STEP 5: unique_ptr<PreparedStatement> destructs
        ↓
        C++ sends to MySQL over TCP:
          COM_STMT_CLOSE packet
          Statement ID: 42
        MySQL frees the compiled plan for statement 42
```

---

# PART 12 — What COALESCE Does in the SQL

```sql
SELECT id, seat_label, is_booked, COALESCE(booked_by, '') AS booked_by
FROM seats WHERE event_id = 1
```

The `booked_by` column in MySQL is `VARCHAR(255)` with no `NOT NULL`.
That means it can be NULL.

When a seat is unbooked:
```
id=1, seat_label="A1", is_booked=0, booked_by=NULL
```

`COALESCE(booked_by, '')` means:
- If `booked_by` is NOT NULL → return `booked_by`
- If `booked_by` IS NULL → return `''` (empty string)

Without COALESCE, `result->getString("booked_by")` might throw or return
garbage when the value is NULL. With COALESCE, we always get a string.

The JSON output is clean:
```json
{"id":1,"seat_label":"A1","is_booked":false,"booked_by":""}
```
instead of potentially:
```json
{"id":1,"seat_label":"A1","is_booked":false,"booked_by":null}
```

---

# PART 13 — How Docker Connects to the C++ Server

```
Your machine:
  localhost:3306 → Docker maps to → MySQL container:3306

The mapping is defined in docker-compose.yml:
  ports:
    - "3306:3306"
  meaning: host machine port 3306 maps to container port 3306

When C++ code runs:
  url_ = "tcp://127.0.0.1:3306"
  driver_->connect(url_, user_, password_)

  ↓ OS sends TCP packet to 127.0.0.1:3306
  ↓ Docker intercepts it (via iptables NAT rules)
  ↓ Forwards to MySQL container's port 3306
  ↓ MySQL receives the connection
  ↓ MySQL sends greeting packet back through the same path
  ↓ C++ receives it
  ↓ Authentication handshake completes
  ↓ Connection established
```

---

# PART 14 — How the JSON Responses Are Built

There is no JSON library. We build JSON strings manually using `std::ostringstream`.

```cpp
std::ostringstream oss;  // in-memory string buffer

oss << "[";              // writes "[" to buffer

bool first = true;
while (result->next()) {
    if (!first) oss << ",";   // add comma between items (not before first)
    first = false;

    oss << "{"
        << "\"id\":"          << result->getInt("id")    // e.g. 1
        << ","
        << "\"seat_label\":\"" << result->getString("seat_label") << "\""  // e.g. "A1"
        << ","
        << "\"is_booked\":"   << (result->getBoolean("is_booked") ? "true" : "false")
        << ","
        << "\"booked_by\":\"" << result->getString("booked_by") << "\""
        << "}";
}

oss << "]";

std::string json = oss.str();
// "[{"id":1,"seat_label":"A1","is_booked":false,"booked_by":""},{"id":2,...}]"
```

`std::ostringstream` is like a string builder.
Each `<<` appends to an internal buffer.
`oss.str()` returns the whole thing as a `std::string`.

This is more efficient than string concatenation with `+`
because `+` creates a new string object every time.
`ostringstream` writes into one growing buffer.

---

# PART 15 — Complete Data Flow Summary

```
USER ACTION: Click seat B4
     │
     │  Browser JS
     ▼
fetch("POST /book", {body: JSON.stringify({event_id:1, seat_id:14, user:"alice"})})
     │
     │  HTTP over TCP to port 18080
     ▼
Crow HTTP Server
  - accepts TCP connection
  - parses HTTP request
  - matches route "/book"
  - hands to worker Thread 3
     │
     │  C++ route handler
     ▼
crow::json::load(req.body)       → parse JSON
body["seat_id"].i()              → extract seat_id = 14
db.acquire()                     → pop conn2 from pool queue
conn->setAutoCommit(false)       → BEGIN transaction in MySQL
     │
     │  MySQL query over TCP to port 3306
     ▼
SELECT id, is_booked, seat_label
FROM seats WHERE id=14 AND event_id=1
FOR UPDATE
     │
     │  InnoDB
     ▼
  Locate row 14 via primary key index
  Check existing locks: none
  Place EXCLUSIVE LOCK on row 14
  Return row: is_booked=0, seat_label="B4"
     │
     │  Back in C++
     ▼
result->getBoolean("is_booked") = false  → seat available
     │
     │  MySQL query over TCP
     ▼
UPDATE seats
SET is_booked=1, booked_by='alice', booked_at=NOW()
WHERE id=14 AND event_id=1
     │
     │  InnoDB
     ▼
  Update row 14 in transaction buffer
  1 row affected
     │
     │  Back in C++
     ▼
conn->commit()
     │
     │  MySQL
     ▼
  Write to redo log (fsync)
  Release EXCLUSIVE LOCK on row 14
  Commit acknowledged
     │
     │  Back in C++
     ▼
conn->setAutoCommit(true)
PooledConnection destructs → conn2 back to pool
return json_response(200, {"success":true, "seat_label":"B4", ...})
     │
     │  HTTP response over TCP
     ▼
Browser JS
  res.status === 200
  showToast("✓ Seat B4 booked for alice!")
  setTimeout(fetchSeats, 200)
     │
     │  200ms later
     ▼
fetch("GET /seats?event_id=1")
     │  ... same flow as Part 3 ...
     ▼
Seat B4 now shows is_booked=true → renders RED on screen
```


---

# PART 16 — What If a Transaction Never Commits or Rolls Back?

This is one of the most important production concerns with any locking system.

## The Scenario

```
Alice's thread:
  BEGIN TRANSACTION
  SELECT seat B4 FOR UPDATE   ← row B4 is now locked
  ... bug in code, thread hangs, or gets stuck in a loop ...
  ... never calls commit() ...
  ... never calls rollback() ...

Bob's thread:
  SELECT seat B4 FOR UPDATE   ← BLOCKED. Waiting for Alice's lock.
  waiting...
  waiting...
```

The question: **is Bob stuck forever?**

The answer: **No. But only if the system is designed correctly.**

---

## The Right Approach: Don't Rely on MySQL Timeouts Alone

MySQL does have safety nets (`innodb_lock_wait_timeout`, `wait_timeout`)
but relying on them as your primary defense means Bob waits 50 seconds
before getting an error. That is unacceptable in production.

The correct approach is layered prevention:

---

### 1. Keep Transactions Very Short (Most Important Rule)

A transaction should do exactly one thing and finish in milliseconds.

```
GOOD — our current booking transaction:
  BEGIN
    SELECT seat FOR UPDATE    ← ~1ms
    check is_booked           ← ~0ms (C++ memory check)
    UPDATE seat               ← ~1ms
  COMMIT                      ← ~1ms
  Total: ~3ms
```

```
BAD — a transaction that does too much:
  BEGIN
    SELECT seat FOR UPDATE
    call external payment API   ← could take 5 seconds, could time out
    send confirmation email     ← network call
    UPDATE seat
  COMMIT
  Total: unpredictable, potentially minutes
```

The rule: **never make network calls, file I/O, or slow operations inside a transaction.**
Do all slow work BEFORE or AFTER the transaction. The transaction itself should
only contain fast, local database operations.

In our project the transaction is 3 SQL statements — done in ~3ms.
Nobody waits long.

---

### 2. Request Timeouts at the HTTP Layer

Every HTTP request should have a maximum allowed time.
If the handler takes longer than N seconds, it is cancelled.

Our current code has no request timeout. In production you would add one:

```cpp
// Crow supports per-route timeouts (example concept)
// If the handler takes > 5 seconds, Crow cancels it
// The PooledConnection destructor fires → rollback → lock released
```

At the infrastructure level, a load balancer (Nginx, AWS ALB) typically
enforces a request timeout of 30–60 seconds. If the C++ backend doesn't
respond in time, the load balancer closes the connection. Crow detects the
closed connection, cancels the handler, stack unwinds, destructors fire,
transaction is rolled back.

---

### 3. Fix the PooledConnection Destructor (Application-Level Safety Net)

As identified: our current destructor returns the connection to the pool
WITHOUT rolling back any open transaction. This is a bug.

**Current code (has the gap):**
```cpp
PooledConnection::~PooledConnection() {
    if (conn_ && releaser_) {
        releaser_(conn_);   // just returns to pool — open transaction survives!
    }
}
```

**Fixed code (correct):**
```cpp
PooledConnection::~PooledConnection() {
    if (conn_ && releaser_) {
        try {
            // If a transaction is still open (autoCommit is false),
            // roll it back before returning the connection to the pool.
            // This ensures the next user of this connection gets a clean state.
            if (!conn_->getAutoCommit()) {
                conn_->rollback();
                conn_->setAutoCommit(true);
            }
        } catch (...) {
            // Ignore errors during cleanup — we're in a destructor
        }
        releaser_(conn_);   // now safe to return to pool
    }
}
```

With this fix: even if the handler returns normally without calling
`commit()` or `rollback()`, the destructor cleans up.
The next request that acquires this connection always starts fresh.

---

### 4. Explicit catch(…) in Every Handler

Our booking handler already does this:

```cpp
try {
    conn->setAutoCommit(false);
    // ... SELECT FOR UPDATE ...
    // ... UPDATE ...
    conn->commit();
} catch (...) {
    // ANY exception — std::bad_alloc, sql::SQLException, anything
    try { conn->rollback(); } catch (...) {}
    try { conn->setAutoCommit(true); } catch (...) {}
    throw;   // re-throw so the outer catch returns 500 to client
}
```

This is the first line of defence. If anything goes wrong inside the
transaction — exception is thrown, lock is released immediately.

---

### 5. MySQL innodb_lock_wait_timeout (Last Resort)

If all application-level defences fail and Bob is still waiting:

```
Default: innodb_lock_wait_timeout = 50 seconds
```

After 50 seconds, MySQL cancels Bob's waiting query and returns:
```
ERROR 1205: Lock wait timeout exceeded; try restarting transaction
```

Our catch block handles this and returns 500 to Bob.
Bob is unblocked. He can retry.

In production, lower this so users don't wait 50 seconds:
```sql
-- In my.cnf or docker-compose environment:
SET GLOBAL innodb_lock_wait_timeout = 5;
```

Now Bob waits at most 5 seconds before getting a clean error response.

---

### 6. MySQL Automatic Deadlock Detection

A deadlock is different from a stuck transaction. It is a circular wait:

```
Transaction A: holds lock on row 1, wants lock on row 2
Transaction B: holds lock on row 2, wants lock on row 1
Both are waiting for each other. Neither can proceed.
```

MySQL InnoDB detects this automatically using a "wait-for graph":
- InnoDB tracks which transaction is waiting for which lock
- If it detects a cycle (A waits for B, B waits for A), it picks a victim
- The victim (usually the transaction that has done less work) is automatically
  rolled back with error 1213:

```
ERROR 1213: Deadlock found when trying to get lock; try restarting transaction
```

The other transaction proceeds normally and completes.

**Can this happen in our project?**

Our booking handler locks exactly ONE row per transaction.
You cannot have a circular wait with single-row transactions.
Deadlocks require at least two transactions each holding a lock
the other wants — impossible here.

However, if the project grew to book multiple seats in one transaction
(e.g., "book seats B4 AND B5 together"), deadlocks could occur if two
users booked the same pair in opposite order. The fix: always lock rows
in a consistent order (e.g., always by ascending seat ID).

---

## Summary: The Full Defence Stack

```
Alice's transaction is stuck / never commits:

Layer 1: catch(...) in handler         → rollback on any exception     [immediate]
Layer 2: PooledConnection destructor   → rollback if autoCommit=false  [immediate, needs fix]
Layer 3: HTTP request timeout          → cancel handler, unwind stack  [seconds, infra-level]
Layer 4: innodb_lock_wait_timeout      → Bob's query cancelled after N seconds [5–50s]
Layer 5: MySQL wait_timeout            → idle connection closed, transaction rolled back [minutes]
Layer 6: Deadlock detector             → victim auto-rolled back by MySQL [immediate]

The fundamental rule:
  Keep transactions SHORT.
  3 SQL statements. ~3ms. Done.
  The shorter the transaction, the smaller the window for anything to go wrong.
```

---

## What "Short Transactions" Looks Like in Practice

```
WRONG way (long transaction):

  conn->setAutoCommit(false);

  SELECT seat FOR UPDATE          ← lock acquired
  call_payment_api()              ← takes 3 seconds, might time out
  send_email()                    ← takes 1 second
  UPDATE seat SET is_booked=1
  insert_booking_record()
  update_user_credits()

  conn->commit();                 ← lock held for ~4+ seconds
                                     everyone trying to book B4 waits 4 seconds


CORRECT way (short transaction):

  // Do all slow work OUTSIDE the transaction:
  validate_user();                ← no transaction yet
  check_payment_pre_auth();       ← no transaction yet, ~3 seconds

  // Transaction is only the fast database writes:
  conn->setAutoCommit(false);
  SELECT seat FOR UPDATE          ← lock acquired
  UPDATE seat SET is_booked=1     ← ~1ms
  conn->commit();                 ← lock released after ~2ms total

  // Do post-transaction work outside:
  confirm_payment();              ← no lock held
  send_email();                   ← no lock held
```

The lock on B4 is held for ~2ms instead of ~4 seconds.
1000 concurrent users can all complete without significant wait.
