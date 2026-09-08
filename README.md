# Seat Booking System

A high-concurrency seat booking backend built with **C++17**, **Crow**, and **MySQL**.

## Features

- Connection-pooled MySQL access
- Transaction-safe booking with row-level locking (`SELECT ... FOR UPDATE`)
- REST endpoints for seats, booking, reset, and health
- Simple static frontend served from `/`

## Tech Stack

- C++17 + CMake
- [Crow](https://crowcpp.org/)
- MySQL 8.0
- MySQL Connector/C++
- Docker Compose (for local MySQL)

## Project Structure

```text
src/            C++ source files
sql/schema.sql  Database schema and seed data
static/         Frontend files
scripts/setup.sh  Local setup helper
```

## Prerequisites

- CMake 3.15+
- C++ compiler with C++17 support
- `vcpkg` (with `VCPKG_ROOT` set)
- Docker + Docker Compose (recommended for local MySQL)

## Setup and Build

1. Start MySQL:

```bash
docker compose up -d
```

2. Build (using vcpkg toolchain):

```bash
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

> You can also run `scripts/setup.sh` to automate local setup and build.

## Run

```bash
./build/ticketing_engine
```

Default server URL: `http://localhost:18080`

## Environment Variables

| Variable | Default |
|---|---|
| `DB_HOST` | `127.0.0.1` |
| `DB_PORT` | `3306` |
| `DB_USER` | `root` |
| `DB_PASS` | `root` |
| `DB_NAME` | `ticketing` |
| `POOL_SIZE` | `10` |
| `PORT` | `18080` |
| `THREADS` | `4` |

## API Endpoints

- `GET /` — frontend
- `GET /seats?event_id=1` — list seats
- `POST /book` — book seat
- `POST /reset` — reset all seats
- `GET /health` — health check

### Example booking request

```bash
curl -X POST http://localhost:18080/book \
  -H "Content-Type: application/json" \
  -d '{"event_id":1,"seat_id":1,"user":"alice"}'
```
