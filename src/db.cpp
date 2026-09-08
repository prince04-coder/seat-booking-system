#include "db.h"

// ============================================================
// PooledConnection implementation
// ============================================================

PooledConnection::PooledConnection(sql::Connection* conn,
                                   std::function<void(sql::Connection*)> releaser)
    : conn_(conn), releaser_(std::move(releaser)) {}

PooledConnection::~PooledConnection() {
    if (conn_ && releaser_) {
        // Safety net: if a transaction is still open (autoCommit was set to false
        // but commit/rollback was never called), roll it back before returning
        // the connection to the pool.
        //
        // Without this: the next handler that acquires this connection would
        // inherit a dirty open transaction — reading stale locked data.
        try {
            if (!conn_->getAutoCommit()) {
                std::cerr << "[PooledConnection] WARNING: open transaction on return"
                          << " — rolling back automatically." << std::endl;
                conn_->rollback();
                conn_->setAutoCommit(true);
            }
        } catch (...) {
            // Never throw from a destructor — swallow errors silently
        }
        releaser_(conn_);
    }
}

PooledConnection::PooledConnection(PooledConnection&& other) noexcept
    : conn_(other.conn_), releaser_(std::move(other.releaser_)) {
    other.conn_     = nullptr;
    other.releaser_ = nullptr;
}

PooledConnection& PooledConnection::operator=(PooledConnection&& other) noexcept {
    if (this != &other) {
        if (conn_ && releaser_) releaser_(conn_);
        conn_           = other.conn_;
        releaser_       = std::move(other.releaser_);
        other.conn_     = nullptr;
        other.releaser_ = nullptr;
    }
    return *this;
}

// ============================================================
// Db (connection pool) implementation
// ============================================================

sql::Connection* Db::create_connection() {
    sql::Connection* conn = driver_->connect(url_, user_, password_);
    conn->setSchema(database_);
    return conn;
}

Db::Db(const std::string& host, int port,
       const std::string& user, const std::string& password,
       const std::string& database, size_t pool_size)
    : user_(user), password_(password), database_(database), pool_size_(pool_size)
{
    url_    = "tcp://" + host + ":" + std::to_string(port);
    driver_ = sql::mysql::get_mysql_driver_instance();

    for (size_t i = 0; i < pool_size_; ++i) {
        try {
            pool_.push(create_connection());
            std::cout << "[Db] Connection " << (i + 1) << "/" << pool_size_
                      << " ready." << std::endl;
        } catch (const sql::SQLException& e) {
            // Clean up already-created connections before re-throwing
            while (!pool_.empty()) {
                delete pool_.front();
                pool_.pop();
            }
            std::cerr << "[Db] Failed to create connection " << (i + 1)
                      << ": " << e.what() << std::endl;
            throw;
        }
    }
    std::cout << "[Db] Pool ready — " << pool_.size() << " connections." << std::endl;
}

Db::~Db() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!pool_.empty()) {
        delete pool_.front();
        pool_.pop();
    }
}

PooledConnection Db::acquire(int wait_timeout_seconds) {
    std::unique_lock<std::mutex> lock(mutex_);

    // Wait up to wait_timeout_seconds for a free connection.
    // cv_.wait()     → blocks FOREVER if pool is empty
    // cv_.wait_for() → returns false after timeout if pool is still empty
    bool got_conn = cv_.wait_for(
        lock,
        std::chrono::seconds(wait_timeout_seconds),
        [this] { return !pool_.empty(); }
    );

    if (!got_conn) {
        // All connections are busy and none freed up in time.
        // Throw so the route handler can return 503 immediately
        // instead of hanging the user's request forever.
        throw std::runtime_error(
            "Connection pool exhausted — no connection available after "
            + std::to_string(wait_timeout_seconds) + "s. Try again later."
        );
    }

    sql::Connection* conn = pool_.front();
    pool_.pop();

    // Check if MySQL dropped this connection while it was idle
    try {
        if (conn->isClosed()) {
            std::cout << "[Db] Reconnecting stale connection..." << std::endl;
            delete conn;
            conn = create_connection();
        }
    } catch (const sql::SQLException& e) {
        std::cerr << "[Db] Reconnect failed: " << e.what() << std::endl;
        try { delete conn; } catch (...) {}
        conn = create_connection();
    }

    return PooledConnection(conn, [this](sql::Connection* c) { this->release(c); });
}

void Db::release(sql::Connection* conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    pool_.push(conn);
    cv_.notify_one();
}

size_t Db::available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pool_.size();
}
