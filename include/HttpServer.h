#ifndef RPI_PLATFORM_HTTP_SERVER_H
#define RPI_PLATFORM_HTTP_SERVER_H

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#error "cpp-httplib must be built with OpenSSL support (CPPHTTPLIB_OPENSSL_SUPPORT) for HTTPS"
#endif

#include <atomic>
#include <httplib.h>
#include "Database.h"
#include "MqttClient.h"

#define HTTP_PORT               8080
#define HTTP_REQUEST_SUCCESS    200
#define HTTP_BAD_REQUEST        400
#define HTTP_SERVER_ERROR       500

class HttpServer {
public:
    HttpServer(Database& rDb, MqttClient& rMqttClient);
    ~HttpServer() = default;

    /// Non-copyable
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    /// Moveable
    HttpServer(HttpServer&&) = default;
    HttpServer& operator=(HttpServer&&) = delete;

    int start(int port, const std::string& rCertPath, const std::string& rKeyPath,
              std::atomic<bool>& rKeepRunning);

private:
    Database& rDb_;
    MqttClient& rMqttClient_;
    int setupRoutes(httplib::Server& rServer);
};

#endif /* RPI_PLATFORM_HTTP_SERVER_H */
