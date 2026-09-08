#include "crow.h"
#include "db.h"
#include "routes.h"

#include <iostream>
#include <cstdlib>
#include <string>

int main() {
    std::cout << R"(
 ╔══════════════════════════════════════════════════════╗
 ║       High-Concurrency Ticketing Engine              ║
 ║       C++ · MySQL · Crow                             ║
 ╚══════════════════════════════════════════════════════╝
    )" << std::endl;

    auto get_env = [](const char* name, const char* default_val) -> std::string {
        const char* val = std::getenv(name);
        return val ? std::string(val) : std::string(default_val);
    };

    const std::string db_host  = get_env("DB_HOST",      "127.0.0.1");
    const int         db_port  = std::stoi(get_env("DB_PORT",  "3306"));
    const std::string db_user  = get_env("DB_USER",      "root");
    const std::string db_pass  = get_env("DB_PASS",      "root");
    const std::string db_name  = get_env("DB_NAME",      "ticketing");
    const int         pool_sz  = std::stoi(get_env("POOL_SIZE", "10"));
    const int         port     = std::stoi(get_env("PORT",      "18080"));
    const int         threads  = std::stoi(get_env("THREADS",   "4"));

    std::cout << "[Config] MySQL:     " << db_host << ":" << db_port << std::endl;
    std::cout << "[Config] Database:  " << db_name << std::endl;
    std::cout << "[Config] Pool Size: " << pool_sz << std::endl;
    std::cout << "[Config] Port:      " << port    << std::endl;
    std::cout << "[Config] Threads:   " << threads << std::endl;
    std::cout << std::endl;

    // Create the connection pool — connects to MySQL on startup
    std::cout << "[Init] Connecting to MySQL..." << std::endl;
    Db db(db_host, db_port, db_user, db_pass, db_name, pool_sz);

    // Create Crow app
    crow::SimpleApp app;
    register_routes(app, db);

    std::cout << std::endl;
    std::cout << "══════════════════════════════════════════════" << std::endl;
    std::cout << "[Server] Starting on http://0.0.0.0:" << port   << std::endl;
    std::cout << "[Server] Frontend:  http://localhost:" << port << "/" << std::endl;
    std::cout << "[Server] Seats API: http://localhost:" << port << "/seats?event_id=1" << std::endl;
    std::cout << "[Server] Book API:  POST http://localhost:" << port << "/book" << std::endl;
    std::cout << "[Server] Health:    http://localhost:" << port << "/health" << std::endl;
    std::cout << "══════════════════════════════════════════════" << std::endl;

    app.port(port).concurrency(threads).run();

    return 0;
}
