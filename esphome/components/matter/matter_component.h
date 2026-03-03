#pragma once
#include "esphome/core/defines.h"
#ifdef USE_MATTER

#include "esphome/core/component.h"
#include "esphome/core/controller.h"

#include <esp_matter.h>
#include <esp_matter_ota.h>

#include <map>
#include <string>

namespace esphome::matter {

class MatterComponent : public Component, public Controller {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  void set_discriminator(uint16_t discriminator) { this->discriminator_ = discriminator; }
  void set_passcode(uint32_t passcode) { this->passcode_ = passcode; }
  void set_vendor_id(uint16_t vendor_id) { this->vendor_id_ = vendor_id; }
  void set_product_id(uint16_t product_id) { this->product_id_ = product_id; }
  void set_product_name(const char *product_name) { this->product_name_ = product_name; }

  /// Get the Matter node pointer (for adding endpoints externally)
  esp_matter::node_t *get_node() { return this->node_; }

  // ---- Controller callbacks for ESPHome entity state changes ----
  // When an ESPHome entity state changes, these push the new state to Matter attributes.
#ifdef USE_FAN
  void on_fan_update(fan::Fan *obj) override;
#endif
#ifdef USE_SWITCH
  void on_switch_update(switch_::Switch *obj) override;
#endif
#ifdef USE_BINARY_SENSOR
  void on_binary_sensor_update(binary_sensor::BinarySensor *obj) override;
#endif
#ifdef USE_SENSOR
  void on_sensor_update(sensor::Sensor *obj) override;
#endif
#ifdef USE_SELECT
  void on_select_update(select::Select *obj) override;
#endif
#ifdef USE_NUMBER
  void on_number_update(number::Number *obj) override;
#endif

 protected:
  /// Create Matter endpoints for all registered ESPHome entities
  void create_endpoints_();

  /// Create a fan endpoint from an ESPHome fan entity
  uint16_t create_fan_endpoint_(const std::string &name);
  /// Create an on/off plugin unit endpoint from an ESPHome switch entity
  uint16_t create_on_off_endpoint_(const std::string &name);
  /// Create a contact sensor endpoint from an ESPHome binary sensor entity
  uint16_t create_contact_sensor_endpoint_(const std::string &name);
  /// Create a temperature sensor endpoint from an ESPHome temperature sensor
  uint16_t create_temperature_sensor_endpoint_(const std::string &name);
  /// Create a humidity sensor endpoint from an ESPHome humidity sensor
  uint16_t create_humidity_sensor_endpoint_(const std::string &name);

  // ---- Matter SDK callbacks (static, dispatch to instance) ----
  static esp_err_t attribute_update_cb_(esp_matter::attribute::callback_type_t type, uint16_t endpoint_id,
                                        uint32_t cluster_id, uint32_t attribute_id,
                                        esp_matter_attr_val_t *val, void *priv_data);
  static esp_err_t identification_cb_(esp_matter::identification::callback_type_t type, uint16_t endpoint_id,
                                      uint8_t effect_id, uint8_t effect_variant, void *priv_data);
  static void event_cb_(const ChipDeviceEvent *event, intptr_t arg);

  /// Handle a Matter attribute write from a controller (e.g., turn fan on/off)
  esp_err_t handle_attribute_update_(uint16_t endpoint_id, uint32_t cluster_id,
                                     uint32_t attribute_id, esp_matter_attr_val_t *val);

  /// Generate and log the Matter QR code setup payload
  void log_qr_code_();

  // ---- Entity-to-endpoint mapping ----
  // Maps ESPHome entity pointers to Matter endpoint IDs
  std::map<void *, uint16_t> entity_endpoint_map_;
  // Maps Matter endpoint IDs back to ESPHome entity pointers (for reverse lookup)
  std::map<uint16_t, void *> endpoint_entity_map_;

  // Track which endpoints are which type for proper command dispatch
  enum class EndpointType : uint8_t { FAN, ON_OFF, CONTACT_SENSOR, TEMPERATURE_SENSOR, HUMIDITY_SENSOR };
  std::map<uint16_t, EndpointType> endpoint_types_;

  esp_matter::node_t *node_{nullptr};

  uint16_t discriminator_{3840};
  uint32_t passcode_{20202021};
  uint16_t vendor_id_{0xFFF1};
  uint16_t product_id_{0x8000};
  const char *product_name_{nullptr};

  bool matter_started_{false};
};

extern MatterComponent *global_matter;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::matter
#endif  // USE_MATTER
