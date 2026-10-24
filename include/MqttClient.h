#ifndef RPI_PLATFORM_MQTT_CLIENT_H
#define RPI_PLATFORM_MQTT_CLIENT_H

#include <string>
#include <mqtt/async_client.h>
#include "Database.h"

#define MQTT_QOS                    1
#define MQTT_KEEP_ALIVE_S           20
#define MQTT_CONNECT_TIMEOUT_S      10
#define MQTT_RECONNECT_MIN_RETRY_S  1
#define MQTT_RECONNECT_MAX_RETRY_S  30

class MqttClient : private mqtt::callback {
public:
    MqttClient(Database& rDb, const std::string& rServerAddress, const std::string& rClientId);
    ~MqttClient();

    /// Non-copyable
    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;
    /// Non-movable
    MqttClient(MqttClient&&) = delete;
    MqttClient& operator=(MqttClient&&) = delete;

    int connect();
    int disconnect();
    int publishPeripheralCommand(const std::string& rPeripheralName, const std::string& rCommand);

private:
    Database& rDb_;
    mqtt::async_client client_;
    void connected(const std::string& rCause) override;
    void connection_lost(const std::string& rCause) override;
    void message_arrived(mqtt::const_message_ptr pMsg) override;
    void delivery_complete(mqtt::delivery_token_ptr pToken) override;
    int subscribeAll(bool waitForCompletion);
    int handleSensorTopic(const std::string& rSensorName, const std::string& rPayload);
    int handlePeripheralStateTopic(const std::string& rPeripheralName, const std::string& rPayload);
    int publish(const std::string& rTopic, const std::string& rPayload);
};

#endif /* RPI_PLATFORM_MQTT_CLIENT_H */
