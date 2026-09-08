cmake_minimum_required(VERSION 3.15)
project(ticketing_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find dependencies
find_package(Crow CONFIG REQUIRED)
find_package(unofficial-mysql-connector-cpp CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(ticketing_engine
    src/main.cpp
    src/db.cpp
    src/routes.cpp
)

target_include_directories(ticketing_engine PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(ticketing_engine PRIVATE
    Crow::Crow
    mysqlcppconn
    Threads::Threads
)

# Copy static files to build directory
add_custom_command(TARGET ticketing_engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/static
    ${CMAKE_BINARY_DIR}/static
)
