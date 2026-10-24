#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <vector>
#include "HttpServer.h"
#include "MqttClient.h"
#include "Database.h"

// Flipped to false by handleShutdownSignal() on SIGINT/SIGTERM. Has to be //
// a plain global rather than something owned by main() or passed around, //
// because the C signal() API only accepts a free function pointer with no //
// way to bind extra state (no capturing lambdas, no member functions) //
static std::atomic<bool> gTerminateFlag{true};

/**
 * @brief Signal handler for SIGINT/SIGTERM - only touches an atomic flag
 *        and nothing else, since a signal handler may only call
 *        async-signal-safe functions, and a lock-free std::atomic<bool>
 *        store is one of the few operations guaranteed safe there.
 *
 * @param[in] signum Signal number that triggered this handler (SIGINT or
 *                    SIGTERM); not used, both are handled identically.
 */
static void handleShutdownSignal(int signum) {

    gTerminateFlag.store(false);
}

/**
 * @brief Registers a signal handler, reporting failure through a return
 *        code instead of comparing std::signal()'s result directly in an
 *        if (matches this project's error-handling convention).
 *
 * @param[in] signum  Signal number to handle (e.g. SIGINT).
 * @param[in] handler Function to invoke when that signal is raised.
 *
 * @return 0 on success, -1 on failure.
 */
static int registerSignalHandler(int signum, void (*handler)(int)) {

    int ret = 0;
    void (*prevHandler)(int) = std::signal(signum, handler);

    if (prevHandler == SIG_ERR) {
        ret = -1;
    }

    return ret;
}

/**
 * @brief Registers the same handler for every signal number given -
 *        lets main() set up all its shutdown signals with a single call
 *        instead of repeating registerSignalHandler() + error handling
 *        once per signal.
 *
 * @param[in] rSignals Signal numbers to register (e.g. {SIGINT, SIGTERM}).
 * @param[in] handler  Callback to invoke for any of those signals.
 *
 * @return 0 if every signal was registered successfully, -1 if any failed
 *         (registration still proceeds through the rest of the list).
 */
static int registerSignalHandlers(const std::vector<int>& rSignals, void (*handler)(int)) {

    int ret = 0;

    for (size_t i = 0; i < rSignals.size(); i++) {
        int result = registerSignalHandler(rSignals[i], handler);
        if (result != 0) {
            std::cerr << "Failed to register handler for signal " << rSignals[i] << std::endl;
            ret = -1;
        }
    }

    return ret;
}

/**
 * @brief Entry point: opens the database, makes sure the schema exists,
 *        connects the MQTT client to the local broker, then starts the
 *        HTTP server (blocks until the process is killed).
 *
 * @return EXIT_SUCCESS on clean exit, EXIT_FAILURE if the database
 *         couldn't be opened, the schema couldn't be created, the MQTT
 *         client couldn't connect, or the HTTP server failed to start.
 */
int main() {

    // Forces line buffering even when stdout isn't a TTY (e.g. piped to //
    // journald under systemd, which is fully buffered by default) - without //
    // this, log lines sit in the buffer until it fills or the process //
    // exits, instead of showing up in `journalctl -f` as they happen //
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    int ret = 0;

    ret = registerSignalHandlers({SIGINT, SIGTERM}, handleShutdownSignal);
    if (ret != 0) {
        return EXIT_FAILURE;
    }

    try {

        Database db("../data/rpi-platform.db");

        // Creates the sensors/peripherals tables //
        ret = db.createSchema();
        if (ret != 0) {
            std::cerr << "Failed to create schema" << std::endl;
            return EXIT_FAILURE;
        }

        std::cout << "Database ready and schema created" << std::endl;

        // Connects to the local mosquitto broker and subscribes to every //
        // sensor/peripheral topic; keeps receiving messages in the //
        // background (Paho's own thread) for as long as mqttClient stays //
        // in scope - which is why it's declared before the HTTP server, //
        // whose start() blocks until killed //
        MqttClient mqttClient(db, "tcp://localhost:1883", "rpi-platform");

        ret = mqttClient.connect();
        if (ret != 0) {
            std::cerr << "MQTT client failed to connect" << std::endl;
            return EXIT_FAILURE;
        }

        // Binds the HTTP layer to this database instance and the already- //
        // connected MQTT client, so /set-peripheral can forward commands //
        HttpServer server(db, mqttClient);

        // Registers routes and blocks here serving requests until either //
        // the socket fails or gTerminateFlag flips false (SIGINT/SIGTERM) //
        ret = server.start(HTTP_PORT, "../config/server.crt", "../config/server.key", gTerminateFlag);
        if (ret != 0) {
            std::cerr << "HTTP server failed to start" << std::endl;
            return EXIT_FAILURE;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    std::cout << "Exiting cleanly" << std::endl;
    return EXIT_SUCCESS;
}
