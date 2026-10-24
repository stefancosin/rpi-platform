#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include "HttpServer.h"

// How often the shutdown watcher thread in start() re-checks rKeepRunning //
#define SHUTDOWN_WATCHER_POLL_MS 200

/**
 * @brief Binds this server to a database instance and an MQTT client;
 *        nothing that can fail happens here, routes are registered later
 *        in start().
 *
 * @param[in] rDb         Database used by every route handler that needs
 *                        sensor/peripheral data. Must outlive this
 *                        HttpServer.
 * @param[in] rMqttClient MQTT client used to forward peripheral commands
 *                        to the ESP32. Must outlive this HttpServer.
 */
HttpServer::HttpServer(Database& rDb, MqttClient& rMqttClient)
    : rDb_(rDb), rMqttClient_(rMqttClient) {}

/**
 * @brief Reads a static HTML file from disk and writes it to the response
 *        as-is.
 *
 * @param[in]  rPath Filesystem path of the HTML file to serve.
 * @param[out] rRes  HTTP response to populate with the file contents on
 *                    success, or with an error body on failure.
 *
 * @return 0 on success, -1 if the file could not be opened.
 */
static int serveFile(const std::string& rPath, httplib::Response& rRes) {

    std::ifstream file(rPath);
    int ret = 0;

    if (!file.is_open()) {
        rRes.status = HTTP_SERVER_ERROR;
        rRes.set_content("Could not load page", "text/plain");
        ret = -1;
    }

    if (ret == 0) {
        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());

        rRes.status = HTTP_REQUEST_SUCCESS;
        rRes.set_content(content, "text/html");
    }

    return ret;
}

/**
 * @brief Registers every HTTP route this server exposes, grouped by who
 *        calls them: static pages/health for browsers, sensor endpoints
 *        for the ESP32, and peripheral endpoints shared by the control
 *        panel (write) and ESP32 (read).
 *
 * @param[in,out] rServer Server instance to register routes on; not
 *                        started here, just configured.
 *
 * @return 0 on success, -1 on failure.
 */
int HttpServer::setupRoutes(httplib::Server& rServer) {

    // Serve HTML pages //
    rServer.Get("/", [](const httplib::Request&, httplib::Response& rRes) {
        
        int ret = serveFile("../web/webpage.html", rRes);

        if (ret != 0) {
            std::cerr << "Failed to serve /\n";
        }
    });

    rServer.Get("/sensors", [](const httplib::Request&, httplib::Response& rRes) {
        
        int ret = serveFile("../web/sensors.html", rRes);

        if (ret != 0) {
            std::cerr << "Failed to serve /sensors\n";
        }
    });

    rServer.Get("/control", [](const httplib::Request&, httplib::Response& rRes) {

        int ret = serveFile("../web/control.html", rRes);

        if (ret != 0) {
            std::cerr << "Failed to serve /control\n";
        }
    });

    rServer.Get("/auto-config", [](const httplib::Request&, httplib::Response& rRes) {

        int ret = serveFile("../web/auto-config.html", rRes);

        if (ret != 0) {
            std::cerr << "Failed to serve /auto-config\n";
        }
    });

    // Health check //
    rServer.Get("/health", [](const httplib::Request&, httplib::Response& rRes) {
        
        rRes.status = HTTP_REQUEST_SUCCESS;
        rRes.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // Add sensor data sent from ESP32 //
    rServer.Post("/add-sensor", [this](const httplib::Request& rReq,
                                       httplib::Response& rRes) {

        std::string name  = rReq.get_param_value("name");
        std::string value = rReq.get_param_value("value");
        double parsedValue = 0.0;
        // ret: 0 = ok, -1 = bad request, -2 = server/db error //
        int ret = 0;

        if ((name.empty()) || (value.empty())) {
            ret = -1;
        }

        if (ret == 0) {
            try {
                parsedValue = std::stod(value);
            } catch (const std::exception& e) {
                ret = -1;
            }
        }

        if (ret == 0) {
            ret = rDb_.insertSensorReading(name, parsedValue);
            if (ret != 0) {
                ret = -2;
            }
        }

        if (ret == -1) {
            rRes.status = HTTP_BAD_REQUEST;
            rRes.set_content("{\"status\":\"bad request\"}", "application/json");
        } else if (ret == -2) {
            rRes.status = HTTP_SERVER_ERROR;
            rRes.set_content("{\"status\":\"error\"}", "application/json");
        } else {
            rRes.status = HTTP_REQUEST_SUCCESS;
            rRes.set_content("{\"status\":\"ok\"}", "application/json");
        }
    });

    // Get all sensor data //
    rServer.Get("/get-sensors", [this](const httplib::Request&, httplib::Response& rRes) {

        std::vector<SensorReading> readings;
        int ret = rDb_.getAllSensorReadings(readings);

        if (ret == 0) {
            // Hand-build the JSON array to avoid pulling in a JSON library //
            // for four fields; revisit if the payload shape grows //
            std::string result = "[";
            for (size_t i = 0; i < readings.size(); i++) {
                const SensorReading& rReading = readings[i];

                result += "{\"id\":" + std::to_string(rReading.id)
                        + ",\"name\":\"" + rReading.name + "\""
                        + ",\"value\":" + std::to_string(rReading.value)
                        + ",\"timestamp\":\"" + rReading.timestamp + "\"}";

                if (i + 1 < readings.size()) {
                    result += ",";
                }
            }
            result += "]";

            rRes.status = HTTP_REQUEST_SUCCESS;
            rRes.set_content(result, "application/json");
        } else {
            rRes.status = HTTP_SERVER_ERROR;
            rRes.set_content("{\"status\":\"error\"}", "application/json");
        }
    });

    // Set peripheral state - called by control panel //
    rServer.Post("/set-peripheral", [this](const httplib::Request& rReq, httplib::Response& rRes) {

        std::string name  = rReq.get_param_value("name");
        std::string state = rReq.get_param_value("state");

        // ret: 0 = ok, -1 = bad request, -2 = server/db error //
        int ret = 0;

        if (name.empty() || state.empty()) {
            ret = -1;
        }

        if (ret == 0) {
            ret = rDb_.setPeripheralState(name, state);
            if (ret != 0) {
                ret = -2;
            }
        }

        // DB write is the record of intent, publish is what actually moves //
        // the peripheral - both have to succeed for the request to count //
        if (ret == 0) {
            ret = rMqttClient_.publishPeripheralCommand(name, state);
            if (ret != 0) {
                ret = -2;
            }
        }

        if (ret == -1) {
            rRes.status = HTTP_BAD_REQUEST;
            rRes.set_content("{\"status\":\"bad request\"}", "application/json");
        } else if (ret == -2) {
            rRes.status = HTTP_SERVER_ERROR;
            rRes.set_content("{\"status\":\"error\"}", "application/json");
        } else {
            rRes.status = HTTP_REQUEST_SUCCESS;
            rRes.set_content("{\"status\":\"ok\"}", "application/json");
        }
    });

    // Get peripheral state - called by ESP32 //
    rServer.Get("/peripheral-state", [this](const httplib::Request& rReq, httplib::Response& rRes) {

        std::string name = rReq.get_param_value("name");
        std::string state;

        // ret: 0 = ok, -1 = bad request, -2 = server/db error //
        int ret = 0;

        if (name.empty()) {
            ret = -1;
        }

        if (ret == 0) {
            ret = rDb_.getPeripheralState(name, state);
            if (ret != 0) {
                ret = -2;
            }
        }

        if (ret == -1) {
            rRes.status = HTTP_BAD_REQUEST;
            rRes.set_content("{\"status\":\"bad request\"}", "application/json");
        } else if (ret == -2) {
            rRes.status = HTTP_SERVER_ERROR;
            rRes.set_content("{\"status\":\"error\"}", "application/json");
        } else {
            rRes.status = HTTP_REQUEST_SUCCESS;
            rRes.set_content("{\"state\":\"" + state + "\"}", "application/json");
        }
    });

    return 0;
}

/**
 * @brief Loads the TLS certificate, registers routes, and blocks serving
 *        HTTPS requests until either the socket fails or rKeepRunning is
 *        flipped to false from outside (e.g. a signal handler).
 *
 * @param[in]     port         TCP port to listen on.
 * @param[in]     rCertPath    Filesystem path to the TLS certificate (.crt).
 * @param[in]     rKeyPath     Filesystem path to the TLS private key (.key).
 * @param[in,out] rKeepRunning Watched by a background thread for the
 *                             duration of this call; flipping it to false
 *                             makes this method return. Left untouched by
 *                             this method itself.
 *
 * @return 0 on success (only actually returns once listening stops,
 *         gracefully or otherwise); -1 if the certificate/key couldn't be
 *         loaded, routes failed to register, or the socket failed to bind.
 */
int HttpServer::start(int port, const std::string& rCertPath, const std::string& rKeyPath,
                      std::atomic<bool>& rKeepRunning) {

    httplib::SSLServer server(rCertPath.c_str(), rKeyPath.c_str());
    int ret = 0;

    if (!server.is_valid()) {
        std::cerr << "Failed to load TLS certificate/key ("
                   << rCertPath << ", " << rKeyPath << ")\n";
        ret = -1;
    }

    if (ret == 0) {
        ret = setupRoutes(server);
        if (ret != 0) {
            std::cerr << "Failed to setup HTTP routes\n";
        }
    }

    if (ret == 0) {
        // server.listen() below blocks the calling thread until something //
        // else calls server.stop() on it, so a background thread is the //
        // only way to notice rKeepRunning going false while listen() is //
        // running. serverListening covers the other direction: it lets //
        // this same watcher stop itself if listen() returns on its own //
        // (e.g. a runtime error), instead of polling rKeepRunning forever //
        // for a shutdown signal that may never come //
        std::atomic<bool> serverListening{true};

        std::thread watcher([&server, &rKeepRunning, &serverListening]() {
            while ((rKeepRunning.load()) && (serverListening.load())) {
                std::this_thread::sleep_for(std::chrono::milliseconds(SHUTDOWN_WATCHER_POLL_MS));
            }
            server.stop();
        });

        std::cout << "Starting HTTPS server on port " << port << "\n";

        if (!server.listen("0.0.0.0", port)) {
            std::cerr << "Failed to listen on port " << port << "\n";
            ret = -1;
        }

        serverListening.store(false);
        watcher.join();
    }

    return ret;
}
