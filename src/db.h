#pragma once

#include <mysql/jdbc.h>
#include <string>
#include <stdexcept>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <chrono>

// ============================================================
// Simple MySQL connection pool
// ============================================================

// RAII wrapper — automatically returns connection to pool on destruction
// Also rolls back any open transaction on destruction (safety net)
class PooledConnection {
public:
    PooledConnection(sql::Connection* conn,
                     std::function<void(sql::Connection*)> releaser);
    ~PooledConnection();

    // Non-copyable, movable only
    PooledConnection(const PooledConnection&) = delete;
    PooledConnection& operator=(const PooledConnection&) = delete;
    PooledConnection(PooledConnection&& other) noexcept;
    PooledConnection& operator=(PooledConnection&& other) noexcept;

    sql::Connection* operator->() { return conn_; }
    sql::Connection& operator*()  { return *conn_; }
    sql::Connection* get()        { return conn_; }

private:
    sql::Connection* conn_;
    std::function<void(sql::Connection*)> releaser_;
};

// ============================================================
// TransactionGuard — scoped transaction with timeout enforcement
//
// Usage:
//   auto conn = db.acquire();
//   TransactionGuard txn(conn.get(), 5);   // 5 second timeout
//   // ... do SQL ...
//   txn.commit();
//   // if commit() is never called before destruction → auto rollback
//   // if timeout exceeded before commit() → throws std::runtime_error
// ============================================================
class TransactionGuard {
public:
    // Begin a transaction on conn, with a timeout in seconds.
    // If commit() is not called within timeout_seconds, the next
    // SQL operation will detect the deadline and throw.
    TransactionGuard(sql::Connection* conn, int timeout_seconds = 5)
        : conn_(conn),
          committed_(false),
          deadline_(std::chrono::steady_clock::now()
                    + std::chrono::seconds(timeout_seconds))
    {
        conn_->setAutoCommit(false);  // BEGIN transaction
    }

    ~TransactionGuard() {
        // If commit() was never called, roll back automatically
        if (!committed_) {
            try {
                std::cerr << "[TransactionGuard] commit() not called — rolling back."
                          << std::endl;
                conn_->rollback();
                conn_->setAutoCommit(true);
            } catch (...) {}
        }
    }

    // Call before any SQL operation inside the transaction
    // to check if the deadline has been exceeded.
    void check_timeout() {
        if (std::chrono::steady_clock::now() > deadline_) {
            try { conn_->rollback(); } catch (...) {}
            try { conn_->setAutoCommit(true); } catch (...) {}
            committed_ = true;  // prevent double-rollback in destructor
            throw std::runtime_error(
                "Transaction timeout exceeded — rolled back automatically."
            );
        }
    }

    void commit() {
        check_timeout();   // last chance — check before committing
        conn_->commit();
        conn_->setAutoCommit(true);
        committed_ = true;
    }

    void rollback() {
        conn_->rollback();
        conn_->setAutoCommit(true);
        committed_ = true;  // mark done so destructor doesn't double-rollback
    }

    // Non-copyable
    TransactionGuard(const TransactionGuard&) = delete;
    TransactionGuard& operator=(const TransactionGuard&) = delete;

private:
    sql::Connection* conn_;
    bool committed_;
    std::chrono::steady_clock::time_point deadline_;
};

// ============================================================
// Thread-safe MySQL connection pool
// ============================================================
class Db {
public:
    Db(const std::string& host, int port,
       const std::string& user, const std::string& password,
       const std::string& database, size_t pool_size = 10);
    ~Db();

    // Non-copyable
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    // Acquire a connection — blocks if none are available
    // times_out_after: max seconds to wait for an available connection
    PooledConnection acquire(int wait_timeout_seconds = 5);

    size_t available() const;
    size_t pool_size() const { return pool_size_; }

private:
    void release(sql::Connection* conn);
    sql::Connection* create_connection();

    sql::mysql::MySQL_Driver* driver_;  // singleton, do NOT delete
    std::string url_;
    std::string user_;
    std::string password_;
    std::string database_;
    size_t pool_size_;

    std::queue<sql::Connection*> pool_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};
