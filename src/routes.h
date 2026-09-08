#pragma once

#include "crow.h"
#include "db.h"

// Register all HTTP route handlers on the Crow app
void register_routes(crow::SimpleApp& app, Db& db);
