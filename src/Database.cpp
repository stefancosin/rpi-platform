#include <iostream>
#include "Database.h"

/**
 * @brief Opens (or creates) the SQLite database file and tunes it for
 *        concurrent access (WAL journal mode + busy timeout).
 *
 * @param[in] rDbPath Filesystem path to the SQLite database file.
 *
 * @note A constructor has no return value to report failure through, so
 *       unlike every other method in this class, this one throws
 *       SQLite::Exception if the file can't be opened or created.
 */
Database::Database(const std::string& rDbPath)
    : db_(rDbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE) {

    /* WAL lets readers and the writer work concurrently instead of
       blocking each other on every access - matters once the MQTT
       thread and the HTTP thread pool both hit this connection. */
    db_.exec("PRAGMA journal_mode=WAL;");
    db_.exec("PRAGMA busy_timeout=5000;");
}

/**
 * @brief Creates the sensors/peripherals tables if this is a fresh
 *        database file.
 *
 * @return 0 on success, -1 on failure.
 */
int Database::createSchema() {

    std::lock_guard<std::mutex> lock(mutex_);

    try {
        db_.exec(
            "CREATE TABLE IF NOT EXISTS sensors ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "name TEXT NOT NULL, "
            "value REAL NOT NULL, "
            "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);"
        );

        db_.exec(
            "CREATE TABLE IF NOT EXISTS peripherals ("
            "name TEXT PRIMARY KEY, "
            "state TEXT NOT NULL, "
            "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);"
        );
    } catch (const std::exception& e) {
        std::cerr << "createSchema failed: " << e.what() << std::endl;
        return -1;
    }

    return 0;
}

/**
 * @brief Appends one sensor reading (e.g. a value pushed by the ESP32).
 *
 * @param[in] rName Sensor name (e.g. "temp1").
 * @param[in] value Reading value to store.
 *
 * @return 0 on success, -1 on failure.
 */
int Database::insertSensorReading(const std::string& rName, double value) {

    std::lock_guard<std::mutex> lock(mutex_);

    try {
        SQLite::Statement query(db_, "INSERT INTO sensors (name, value) VALUES (?, ?);");
        query.bind(1, rName);
        query.bind(2, value);
        query.exec();
    } catch (const std::exception& e) {
        std::cerr << "insertSensorReading failed: " << e.what() << std::endl;
        return -1;
    }

    return 0;
}

/**
 * @brief Retrieves every stored sensor reading, oldest first.
 *
 * @param[out] rOutReadings Filled with all sensor readings on success;
 *                          left untouched on failure.
 *
 * @return 0 on success, -1 on failure.
 */
int Database::getAllSensorReadings(std::vector<SensorReading>& rOutReadings) {

    std::lock_guard<std::mutex> lock(mutex_);

    try {
        std::vector<SensorReading> readings;

        SQLite::Statement query(db_, "SELECT id, name, value, timestamp FROM sensors;");
        while (query.executeStep()) {
            readings.push_back(SensorReading{
                query.getColumn(0).getInt(),
                query.getColumn(1).getString(),
                query.getColumn(2).getDouble(),
                query.getColumn(3).getString()
            });
        }

        // Only touch the output param once we know the whole query succeeded //
        rOutReadings = std::move(readings);
    } catch (const std::exception& e) {
        std::cerr << "getAllSensorReadings failed: " << e.what() << std::endl;
        return -1;
    }

    return 0;
}

/**
 * @brief Inserts or updates the current state of one peripheral
 *        (upsert by name).
 *
 * @param[in] rName  Peripheral name (e.g. "heater").
 * @param[in] rState New state to store for that peripheral.
 *
 * @return 0 on success, -1 on failure.
 */
int Database::setPeripheralState(const std::string& rName, const std::string& rState) {

    std::lock_guard<std::mutex> lock(mutex_);

    try {
        SQLite::Statement query(db_,
            "INSERT INTO peripherals (name, state) VALUES (?, ?) "
            "ON CONFLICT(name) DO UPDATE SET state=excluded.state, "
            "timestamp=CURRENT_TIMESTAMP;");
        query.bind(1, rName);
        query.bind(2, rState);
        query.exec();
    } catch (const std::exception& e) {
        std::cerr << "setPeripheralState failed: " << e.what() << std::endl;
        return -1;
    }

    return 0;
}

/**
 * @brief Retrieves the current state of one peripheral.
 *
 * @param[in]  rName     Peripheral name to look up.
 * @param[out] rOutState Filled with the stored state on success, or with
 *                       "0" if the peripheral has no recorded state yet
 *                       (not treated as a failure).
 *
 * @return 0 on success, -1 on failure.
 */
int Database::getPeripheralState(const std::string& rName, std::string& rOutState) {

    std::lock_guard<std::mutex> lock(mutex_);

    try {
        SQLite::Statement query(db_, "SELECT state FROM peripherals WHERE name = ?;");
        query.bind(1, rName);

        if (query.executeStep()) {
            rOutState = query.getColumn(0).getString();
        } else {
            rOutState = "0";
        }
    } catch (const std::exception& e) {
        std::cerr << "getPeripheralState failed: " << e.what() << std::endl;
        return -1;
    }

    return 0;
}
