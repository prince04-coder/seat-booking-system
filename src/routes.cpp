#include "routes.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>

// Helper: build a JSON response with CORS header
static crow::response json_response(int status, const std::string& body) {
    auto res = crow::response(status);
    res.set_header("Content-Type", "application/json");
    res.set_header("Access-Control-Allow-Origin", "*");
    res.body = body;
    return res;
}

void register_routes(crow::SimpleApp& app, Db& db) {

    // ─────────────────────────────────────────────────
    // GET / — Serve the static frontend
    // ─────────────────────────────────────────────────
    CROW_ROUTE(app, "/")
    ([](const crow::request&) {
        std::vector<std::string> paths = {
            "static/index.html",
            "../static/index.html",
            "../../static/index.html"
        };
        for (const auto& path : paths) {
            std::ifstream file(path);
            if (file.is_open()) {
                std::ostringstream ss;
                ss << file.rdbuf();
                auto res = crow::response(200);
                res.set_header("Content-Type", "text/html; charset=utf-8");
                res.body = ss.str();
                return res;
            }
        }
        return crow::response(404, "Frontend not found. Ensure static/index.html exists.");
    });

    // ─────────────────────────────────────────────────
    // GET /seats?event_id=1 — List all seats
    // ─────────────────────────────────────────────────
    CROW_ROUTE(app, "/seats")
    ([&db](const crow::request& req) {
        auto event_id_str = req.url_params.get("event_id");
        int event_id = event_id_str ? std::stoi(event_id_str) : 1;

        try {
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

            std::ostringstream oss;
            oss << "[";
            bool first = true;
            while (result->next()) {
                if (!first) oss << ",";
                first = false;
                oss << "{"
                    << "\"id\":"         << result->getInt("id")                          << ","
                    << "\"seat_label\":\"" << result->getString("seat_label")              << "\","
                    << "\"is_booked\":"  << (result->getBoolean("is_booked") ? "true" : "false") << ","
                    << "\"booked_by\":\"" << result->getString("booked_by")                << "\""
                    << "}";
            }
            oss << "]";

            return json_response(200, oss.str());

        } catch (const sql::SQLException& e) {
            std::cerr << "[/seats] MySQL error: " << e.what() << std::endl;
            return json_response(500, R"({"error":"Database error"})");
        }
    });

    // ─────────────────────────────────────────────────
    // POST /book — Book a seat (with transaction)
    //
    // Flow:
    //   1. Parse JSON body (event_id, seat_id, user)
    //   2. Acquire connection from pool
    //   3. setAutoCommit(false) — begin transaction
    //   4. SELECT ... FOR UPDATE — lock the row
    //   5. Check is_booked
    //   6. UPDATE if available → COMMIT
    //   7. ROLLBACK on conflict or error
    // ─────────────────────────────────────────────────
    CROW_ROUTE(app, "/book").methods("POST"_method, "OPTIONS"_method)
    ([&db](const crow::request& req) {
        // Handle CORS preflight
        if (req.method == crow::HTTPMethod::Options) {
            auto res = crow::response(204);
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            return res;
        }

        // Parse JSON body
        auto body = crow::json::load(req.body);
        if (!body) {
            return json_response(400, R"({"error":"Invalid JSON body"})");
        }

        int event_id, seat_id;
        std::string user;
        try {
            event_id = static_cast<int>(body["event_id"].i());
            seat_id  = static_cast<int>(body["seat_id"].i());
            user     = body["user"].s();
        } catch (...) {
            return json_response(400,
                R"({"error":"Missing required fields: event_id, seat_id, user"})");
        }

        if (user.empty()) {
            return json_response(400, R"({"error":"User name cannot be empty"})");
        }

        try {
            auto conn = db.acquire(5);  // wait max 5 seconds for a free connection
                                        // throws std::runtime_error if pool exhausted

            // TransactionGuard begins the transaction (setAutoCommit false) and
            // enforces a 5-second deadline. If txn.commit() is not called within
            // 5 seconds, the destructor automatically rolls back — even if no
            // exception was thrown. This prevents indefinitely open transactions.
            TransactionGuard txn(conn.get(), 5);

            // SELECT FOR UPDATE — acquires exclusive row-level lock in InnoDB.
            // check_timeout() verifies the 5s deadline hasn't passed before
            // each SQL call.
            txn.check_timeout();
            std::unique_ptr<sql::PreparedStatement> sel(
                conn->prepareStatement(
                    "SELECT id, is_booked, seat_label "
                    "FROM seats WHERE id = ? AND event_id = ? "
                    "FOR UPDATE"
                )
            );
            sel->setInt(1, seat_id);
            sel->setInt(2, event_id);
            std::unique_ptr<sql::ResultSet> result(sel->executeQuery());

            if (!result->next()) {
                txn.rollback();
                return json_response(404, R"({"error":"Seat not found"})");
            }

            bool        is_booked  = result->getBoolean("is_booked");
            std::string seat_label = result->getString("seat_label");

            if (is_booked) {
                txn.rollback();
                return json_response(409,
                    R"({"error":"Seat already booked","seat_label":")" + seat_label + R"("})");
            }

            // Update the seat
            txn.check_timeout();
            std::unique_ptr<sql::PreparedStatement> upd(
                conn->prepareStatement(
                    "UPDATE seats SET is_booked = 1, booked_by = ?, booked_at = NOW() "
                    "WHERE id = ? AND event_id = ?"
                )
            );
            upd->setString(1, user);
            upd->setInt(2, seat_id);
            upd->setInt(3, event_id);
            upd->executeUpdate();

            // Commit — releases the row-level lock
            // check_timeout() runs one final time inside commit()
            txn.commit();

            std::cout << "[/book] Seat " << seat_label << " booked by " << user << std::endl;

            return json_response(200,
                R"({"success":true,"message":"Seat booked successfully","seat_label":")" +
                seat_label + R"(","user":")" + user + R"("})");

        } catch (const std::runtime_error& e) {
            // Covers: pool exhausted timeout + transaction timeout
            std::cerr << "[/book] Timeout: " << e.what() << std::endl;
            return json_response(503, R"({"error":"Server busy, please try again"})");
        } catch (const sql::SQLException& e) {
            std::cerr << "[/book] MySQL error: " << e.what() << std::endl;
            return json_response(500, R"({"error":"Database error"})");
        }
    });

    // ─────────────────────────────────────────────────
    // POST /reset — Reset all seats (dev/testing)
    // ─────────────────────────────────────────────────
    CROW_ROUTE(app, "/reset").methods("POST"_method, "OPTIONS"_method)
    ([&db](const crow::request& req) {
        if (req.method == crow::HTTPMethod::Options) {
            auto res = crow::response(204);
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            return res;
        }

        try {
            auto conn = db.acquire();
            std::unique_ptr<sql::Statement> stmt(conn->createStatement());
            stmt->executeUpdate(
                "UPDATE seats SET is_booked = 0, booked_by = NULL, booked_at = NULL"
            );
            std::cout << "[/reset] All seats reset." << std::endl;
            return json_response(200, R"({"success":true,"message":"All seats have been reset"})");
        } catch (const sql::SQLException& e) {
            std::cerr << "[/reset] MySQL error: " << e.what() << std::endl;
            return json_response(500, R"({"error":"Database error"})");
        }
    });

    // ─────────────────────────────────────────────────
    // GET /health — Health check
    // ─────────────────────────────────────────────────
    CROW_ROUTE(app, "/health")
    ([&db]() {
        bool ok = db.available() > 0;

        std::ostringstream oss;
        oss << "{"
            << "\"status\":\""         << (ok ? "healthy" : "degraded") << "\","
            << "\"pool_available\":"   << db.available()                 << ","
            << "\"pool_total\":"       << db.pool_size()
            << "}";

        auto res = crow::response(ok ? 200 : 503);
        res.set_header("Content-Type", "application/json");
        res.body = oss.str();
        return res;
    });
}
