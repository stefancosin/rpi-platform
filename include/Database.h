#ifndef RPI_PLATFORM_DATABASE_H
#define RPI_PLATFORM_DATABASE_H

#include <SQLiteCpp/SQLiteCpp.h>
#include <mutex>
#include <string>
#include <vector>

// One row from the sensors table //
struct SensorReading {
    int         id;
    std::string name;
    double      value;
    std::string timestamp;
};

class Database {
public:
    explicit Database(const std::string& rDbPath);
    ~Database() = default;

    /// Non-copyable
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    /// Non-movable
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    int createSchema();
    int insertSensorReading(const std::string& rName, double value);
    int getAllSensorReadings(std::vector<SensorReading>& rOutReadings);
    int setPeripheralState(const std::string& rName, const std::string& rState);
    int getPeripheralState(const std::string& rName, std::string& rOutState);

private:
    SQLite::Database db_;
    std::mutex mutex_;
};

#endif /* RPI_PLATFORM_DATABASE_H */
