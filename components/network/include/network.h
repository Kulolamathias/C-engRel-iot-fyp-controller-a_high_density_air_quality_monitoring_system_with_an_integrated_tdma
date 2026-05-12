/**
 * @file components/network/network.h
 * @brief Common network module – WiFi, MQTT client, NTP, and node ID.
 *
 * =============================================================================
 * ARCHITECTURAL ROLE
 * =============================================================================
 * This module provides a single entry point, network_start(), that initialises
 * the TCP/IP stack, connects to a pre‑configured WiFi access point, starts an
 * MQTT client, and synchronises the system clock via SNTP.
 *
 * It also provides lightweight wrappers for MQTT publish/subscribe and a
 * function to obtain a unique node identifier derived from the ESP32 MAC address.
 *
 * =============================================================================
 * OWNERSHIP
 * =============================================================================
 * - Owns: static MQTT client handle, static node ID buffer.
 * - Provides: network_start(), network_mqtt_publish(), network_mqtt_subscribe(),
 *            network_mqtt_set_data_callback(), network_get_node_id().
 * - Does NOT: interpret message contents, manage TDMA slots, or interact with
 *   sensors/displays.
 *
 * =============================================================================
 * INVARIANTS
 * =============================================================================
 * - network_start() must be called exactly once and returns only after all
 *   sub‑systems are operational.
 * - Credentials are hard‑coded; no runtime configuration is expected.
 *
 * @author AI Assistant
 * @date 2026/05/12
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MQTT data callback type.
 *
 * @param topic   Topic string (null‑terminated).
 * @param data    Payload bytes (NOT null‑terminated).
 * @param data_len Payload length in bytes.
 */
typedef void (*network_mqtt_data_cb_t)(const char *topic,
                                       const char *data,
                                       size_t data_len);

/**
 * @brief Initialise and start all network services.
 *
 * Performs the following steps in order:
 * 1. Initialise NVS (required by WiFi).
 * 2. Create the default event loop and TCP/IP stack.
 * 3. Connect to the hard‑coded WiFi access point (blocks until connected).
 * 4. Initialise and start the MQTT client, connecting to the broker.
 * 5. Synchronise the system clock via SNTP (blocks until time is set).
 *
 * After this function returns, the node is ready for MQTT communication
 * and can rely on a correct UTC timestamp.
 *
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t network_start(void);

/**
 * @brief Publish a message to an MQTT topic.
 *
 * This function is a thin wrapper around esp_mqtt_client_publish().
 *
 * @param topic    Topic string (null‑terminated).
 * @param data     Payload bytes.
 * @param data_len Payload length in bytes.
 * @param qos      Quality of Service (0, 1, or 2).
 * @param retain   Retain flag.
 * @return ESP_OK on success, or an error.
 */
esp_err_t network_mqtt_publish(const char *topic,
                               const char *data,
                               size_t data_len,
                               int qos,
                               bool retain);

/**
 * @brief Subscribe to an MQTT topic.
 *
 * @param topic Topic string (null‑terminated).
 * @param qos   Quality of Service (0, 1, or 2).
 * @return ESP_OK on success, or an error.
 */
esp_err_t network_mqtt_subscribe(const char *topic, int qos);

/**
 * @brief Register a callback for incoming MQTT data.
 *
 * Only one callback can be registered; a second call replaces the previous one.
 * The callback is invoked from the MQTT client task.
 *
 * @param cb Callback function, or NULL to unregister.
 */
void network_mqtt_set_data_callback(network_mqtt_data_cb_t cb);

/**
 * @brief Get the node's unique identifier.
 *
 * The ID is derived from the ESP32 MAC address in the format PREFIX_XXXX,
 * where XXXX is the last four hexadecimal digits of the MAC.
 *
 * @param buf    Output buffer for the ID (will be null‑terminated).
 * @param buflen Size of the output buffer (at least 16 characters recommended).
 * @param prefix The prefix string, e.g. "TX" or "RX".
 */
void network_get_node_id(char *buf, size_t buflen, const char *prefix);

#ifdef __cplusplus
}
#endif