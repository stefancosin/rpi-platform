#include <iostream>
#include <sstream>
#include <vector>
#include "MqttClient.h"

// Topics we subscribe to - '+' is the MQTT single-level wildcard, so this
// matches any sensor/peripheral name without us having to know it upfront //
#define SENSOR_TOPIC_FILTER             "incubator/sensors/+"
#define PERIPHERAL_STATE_TOPIC_FILTER   "incubator/peripherals/+/state"

// Prefix/suffix used to build the topic a command for a given peripheral is
// published to, e.g. "incubator/peripherals/heater/set" //
#define PERIPHERAL_SET_TOPIC_PREFIX     "incubator/peripherals/"
#define PERIPHERAL_SET_TOPIC_SUFFIX     "/set"

/**
 * @brief Splits an MQTT topic string into its '/'-separated segments.
 *
 * @param[in] rTopic Topic string as received from the broker
 *                    (e.g. "incubator/sensors/temp1").
 *
 * @return The segments in order (e.g. {"incubator", "sensors", "temp1"}).
 */
static std::vector<std::string> splitTopic(const std::string& rTopic) {

    std::vector<std::string> tokens;
    std::stringstream stream(rTopic);
    std::string token;

    while (std::getline(stream, token, '/')) {
        tokens.push_back(token);
    }

    return tokens;
}

/**
 * @brief Builds the underlying Paho client for the given broker address;
 *        does not connect yet, see connect().
 *
 * @param[in] rDb            Database used to persist incoming sensor
 *                            readings/peripheral state. Must outlive this
 *                            MqttClient.
 * @param[in] rServerAddress Broker address (e.g. "tcp://localhost:1883").
 * @param[in] rClientId      MQTT client identifier to present to the broker.
 *
 * @note Like Database::Database(), a constructor has no return value to
 *       report failure through, so this one throws mqtt::exception if the
 *       server address is malformed.
 */
MqttClient::MqttClient(Database& rDb, const std::string& rServerAddress,
                        const std::string& rClientId)
    : rDb_(rDb), client_(rServerAddress, rClientId) {
}

/**
 * @brief Disconnects from the broker on the way out - RAII cleanup, so
 *        this runs no matter which path a MqttClient goes out of scope
 *        through (normal return, an early return, or stack unwinding from
 *        an exception), not just the one path where main() happens to
 *        call disconnect() itself. A destructor can't report failure or
 *        let an exception escape, so this just calls disconnect(), which
 *        already logs internally and never throws.
 */
MqttClient::~MqttClient() {
    disconnect();
}

/**
 * @brief Connects to the broker and subscribes to every topic this
 *        platform cares about (see SENSOR_TOPIC_FILTER,
 *        PERIPHERAL_STATE_TOPIC_FILTER). Registers this object as the
 *        callback target, so message_arrived() starts firing once this
 *        returns successfully.
 *
 * @return 0 on success, -1 if the connection or a subscription failed.
 *
 * @note Automatic reconnect is enabled (MQTT_RECONNECT_MIN_RETRY_S..
 *       MQTT_RECONNECT_MAX_RETRY_S backoff), so a dropped connection is
 *       retried by Paho's own thread without this method being called
 *       again. Since set_clean_session(true) means the broker forgets our
 *       subscriptions across reconnects, connected() re-subscribes every
 *       time it fires, including the first time (right after this call).
 */
int MqttClient::connect() {

    int ret = 0;

    mqtt::connect_options connOpts;
    connOpts.set_clean_session(true);
    connOpts.set_keep_alive_interval(MQTT_KEEP_ALIVE_S);
    connOpts.set_connect_timeout(MQTT_CONNECT_TIMEOUT_S);
    connOpts.set_automatic_reconnect(MQTT_RECONNECT_MIN_RETRY_S, MQTT_RECONNECT_MAX_RETRY_S);

    client_.set_callback(*this);

    try {
        client_.connect(connOpts)->wait();
    } catch (const std::exception& e) {
        std::cerr << "MqttClient::connect: failed to connect to broker: " << e.what() << "\n";
        ret = -1;
    }

    if (ret == 0) {
        // Blocking is safe here - this runs on the calling thread, not on
        // any of Paho's own threads (see subscribeAll()'s @note) //
        ret = subscribeAll(true);
    }

    if (ret == 0) {
        std::cout << "MQTT client connected and subscribed\n";
    }

    return ret;
}

/**
 * @brief Cleanly disconnects from the broker - sends a proper MQTT
 *        DISCONNECT packet instead of just dropping the TCP connection
 *        (which is what happens if this is never called and the process
 *        exits/gets killed anyway; the broker only notices after the
 *        keep-alive interval).
 *
 * @return 0 on success, -1 on failure.
 */
int MqttClient::disconnect() {

    int ret = 0;

    try {
        client_.disconnect()->wait();
    } catch (const std::exception& e) {
        std::cerr << "MqttClient::disconnect: " << e.what() << "\n";
        ret = -1;
    }

    return ret;
}

/**
 * @brief Subscribes to every topic this platform cares about (see
 *        SENSOR_TOPIC_FILTER, PERIPHERAL_STATE_TOPIC_FILTER). Factored out
 *        of connect() so connected() can trigger the same two subscribes
 *        on every automatic reconnect, not just the first connect.
 *
 * @param[in] waitForCompletion Whether to block until the broker has
 *                               acknowledged both subscriptions.
 *
 * @note Must be called with waitForCompletion=false from connected() (or
 *       any other callback Paho invokes on its own threads). Those
 *       callbacks run ON Paho's internal receive thread, which is also
 *       the thread responsible for delivering the SUBACK that a blocking
 *       wait() here would be waiting for - so waiting there means the
 *       thread waits on itself and deadlocks (confirmed by hand: hung the
 *       server this way once already). waitForCompletion=true is only
 *       safe from connect(), which runs on the caller's own thread.
 *
 * @return 0 on success, -1 if a subscription failed. When
 *         waitForCompletion=false, this only reports whether the
 *         subscribe request was sent, not whether the broker acked it.
 */
int MqttClient::subscribeAll(bool waitForCompletion) {

    int ret = 0;

    try {
        if (waitForCompletion) {
            client_.subscribe(SENSOR_TOPIC_FILTER, MQTT_QOS)->wait();
            client_.subscribe(PERIPHERAL_STATE_TOPIC_FILTER, MQTT_QOS)->wait();
        } else {
            client_.subscribe(SENSOR_TOPIC_FILTER, MQTT_QOS);
            client_.subscribe(PERIPHERAL_STATE_TOPIC_FILTER, MQTT_QOS);
        }
    } catch (const std::exception& e) {
        std::cerr << "MqttClient::subscribeAll: failed to subscribe: " << e.what() << "\n";
        ret = -1;
    }

    return ret;
}

/**
 * @brief Called by Paho's network thread once the client is connected -
 *        both right after the initial connect() and after every automatic
 *        reconnect. Re-subscribes every time, since set_clean_session(true)
 *        means the broker doesn't remember our subscriptions across a
 *        reconnect. On the very first connect this races harmlessly with
 *        connect()'s own blocking subscribeAll(true) call - the broker
 *        just sees the same subscription requested twice.
 *
 * @param[in] rCause Library-supplied description of what triggered this
 *                    connect (empty on the very first connect).
 */
void MqttClient::connected(const std::string& rCause) {

    int ret = subscribeAll(false);

    if (ret != 0) {
        std::cerr << "MqttClient::connected: re-subscribe failed after reconnect\n";
    }
}

/**
 * @brief Called by Paho's network thread when the broker connection drops
 *        unexpectedly. Only logs - automatic reconnect (configured in
 *        connect()) handles getting back online, and connected() handles
 *        re-subscribing once it does.
 *
 * @param[in] rCause Broker/library-supplied reason for the disconnect.
 */
void MqttClient::connection_lost(const std::string& rCause) {
    std::cerr << "MQTT connection lost: " << rCause << "\n";
}

/**
 * @brief Called by Paho's network thread for every message on a
 *        subscribed topic. Identifies which topic pattern matched and
 *        hands off to the matching handler.
 *
 * @param[in] pMsg Message received from the broker (topic + payload).
 */
void MqttClient::message_arrived(mqtt::const_message_ptr pMsg) {

    std::string topic   = pMsg->get_topic();
    std::string payload = pMsg->to_string();
    std::vector<std::string> tokens = splitTopic(topic);
    int ret = 0;

    if ((tokens.size() == 3) && (tokens[0] == "incubator") && (tokens[1] == "sensors")) {
        ret = handleSensorTopic(tokens[2], payload);
    } else if ((tokens.size() == 4) && (tokens[0] == "incubator")
               && (tokens[1] == "peripherals") && (tokens[3] == "state")) {
        ret = handlePeripheralStateTopic(tokens[2], payload);
    } else {
        std::cerr << "message_arrived: unrecognized topic \"" << topic << "\"\n";
        ret = -1;
    }

    if (ret != 0) {
        std::cerr << "message_arrived: failed to handle message on topic \"" << topic << "\"\n";
    }
}

/**
 * @brief Called by Paho's network thread once a message we published has
 *        been fully delivered/acknowledged. Empty for now - we don't
 *        publish anything yet (that arrives with the HTTP-to-MQTT command
 *        path), but the override must exist since mqtt::callback declares
 *        it.
 *
 * @param[in] pToken Delivery token identifying which publish completed.
 */
void MqttClient::delivery_complete(mqtt::delivery_token_ptr pToken) {
}

/**
 * @brief Parses a sensor reading payload and stores it.
 *
 * @param[in] rSensorName Sensor name extracted from the topic
 *                        (e.g. "temp1").
 * @param[in] rPayload    Raw message payload, expected to be a plain
 *                        number (e.g. "23.5").
 *
 * @return 0 on success, -1 if the payload wasn't a valid number or the
 *         database write failed.
 */
int MqttClient::handleSensorTopic(const std::string& rSensorName, const std::string& rPayload) {

    double value = 0.0;
    int ret = 0;

    try {
        value = std::stod(rPayload);
    } catch (const std::exception& e) {
        std::cerr << "handleSensorTopic: invalid payload \"" << rPayload
                   << "\" for sensor \"" << rSensorName << "\": " << e.what() << "\n";
        ret = -1;
    }

    if (ret == 0) {
        ret = rDb_.insertSensorReading(rSensorName, value);
    }

    return ret;
}

/**
 * @brief Stores a peripheral's reported state.
 *
 * @param[in] rPeripheralName Peripheral name extracted from the topic
 *                            (e.g. "heater").
 * @param[in] rPayload        Raw message payload, stored as-is
 *                            (e.g. "on").
 *
 * @return 0 on success, -1 on failure.
 */
int MqttClient::handlePeripheralStateTopic(const std::string& rPeripheralName,
                                           const std::string& rPayload) {
    int ret = rDb_.setPeripheralState(rPeripheralName, rPayload);
    return ret;
}

/**
 * @brief Publishes a command to a specific peripheral's "set" topic
 *        (e.g. "incubator/peripherals/heater/set"), for the ESP32 to pick
 *        up via its own subscription.
 *
 * @param[in] rPeripheralName Peripheral to command (e.g. "heater").
 * @param[in] rCommand        Command payload to send (e.g. "on").
 *
 * @return 0 on success, -1 on failure.
 */
int MqttClient::publishPeripheralCommand(const std::string& rPeripheralName,
                                          const std::string& rCommand) {

    std::string topic = std::string(PERIPHERAL_SET_TOPIC_PREFIX) + rPeripheralName
                       + PERIPHERAL_SET_TOPIC_SUFFIX;
    int ret = publish(topic, rCommand);

    return ret;
}

/**
 * @brief Low-level publish to an arbitrary topic - the one place this
 *        class talks to the broker to send a message, used by every
 *        higher-level publish* method.
 *
 * @param[in] rTopic   Topic to publish to.
 * @param[in] rPayload Message payload to send.
 *
 * @return 0 on success, -1 if the publish failed.
 */
int MqttClient::publish(const std::string& rTopic, const std::string& rPayload) {

    int ret = 0;

    try {
        client_.publish(rTopic, rPayload, MQTT_QOS, false)->wait();
    } catch (const std::exception& e) {
        std::cerr << "MqttClient::publish: failed to publish to \"" << rTopic << "\": " << e.what() << "\n";
        ret = -1;
    }

    return ret;
}
