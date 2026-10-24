cmake_minimum_required(VERSION 3.10)
project(rpi-platform)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -pthread")

# Include directories (our own headers only - third-party libs come via pkg-config/find_package)
include_directories(include)

# Find SQLiteCpp (system package: libsqlitecpp-dev) - pulls in SQLite3 + Threads transitively
find_package(SQLiteCpp REQUIRED)

# Find cpp-httplib (system package: libcpp-httplib-dev), built with OpenSSL support
find_package(PkgConfig REQUIRED)
pkg_check_modules(HTTPLIB REQUIRED IMPORTED_TARGET cpp-httplib)

# Find Paho MQTT C++ (system package: libpaho-mqttpp-dev, pulls in libpaho-mqtt-dev)
find_package(PahoMqttCpp REQUIRED)

# Add executable
add_executable(rpi-platform
    src/RPIPlatform.cpp
    src/HttpServer.cpp
    src/Database.cpp
    src/MqttClient.cpp
)

# Include headers
target_include_directories(rpi-platform PRIVATE include)

# Link SQLiteCpp, httplib, Paho MQTT C++ and pthread
target_link_libraries(rpi-platform PRIVATE SQLiteCpp PkgConfig::HTTPLIB PahoMqttCpp::paho-mqttpp3 pthread)
