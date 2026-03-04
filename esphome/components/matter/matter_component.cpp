#include "esphome/core/defines.h"
#ifdef USE_MATTER

#include "matter_component.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <esp_matter.h>
#include <esp_matter_endpoint.h>

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <platform/CommissionableDataProvider.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <platform/ESP32/route_hook/ESP32RouteHook.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <setup_payload/SetupPayload.h>

#include <nvs_flash.h>

#ifdef USE_WIFI
#include <mdns.h>
#endif

#ifdef USE_MATTER_THREAD
#include <esp_event.h>
#include <esp_openthread_types.h>
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

static const char *const TAG = "matter";

// We intentionally do NOT use `using namespace esp_matter::endpoint;` because
// it brings in names like `fan`, `contact_sensor`, `temperature_sensor` that
// collide with ESPHome's own namespaces (esphome::fan, esphome::sensor, etc.).
// Instead, we fully qualify all esp_matter calls.

using namespace chip::app::Clusters;

namespace esphome::matter {

MatterComponent *global_matter =  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
    nullptr;

float MatterComponent::get_setup_priority() const {
  // Run after network (WiFi/Thread) is set up but before most other components
  return setup_priority::AFTER_WIFI - 1.0f;
}

void MatterComponent::setup() {
  global_matter = this;

  ESP_LOGI(TAG, "Initializing Matter...");

  // Initialize NVS (Matter uses it for fabric/credential storage)
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS partition needs erase, erasing...");
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

#ifdef USE_WIFI
  // Initialize ESP-IDF mDNS for the CHIP stack (WiFi mode only).
  // ESPHome's mDNS component is skipped (USE_MATTER guard in mdns_esp32.cpp)
  // because CHIP manages its own mDNS service registrations. But CHIP's
  // platform layer expects the ESP-IDF mdns service to be initialized.
  err = mdns_init();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mDNS init failed: %s (may already be initialized)", esp_err_to_name(err));
  } else {
    mdns_hostname_set(App.get_name().c_str());
  }
#endif

#ifdef USE_MATTER_THREAD
  // Thread mode: configure the OpenThread platform for CHIP's ThreadStackManager.
  // CHIP's ThreadStackManagerImpl::_InitThreadStack() calls openthread_init_stack()
  // which requires set_openthread_platform_config() to have been called first.
  // CHIP handles all OpenThread initialization (esp_openthread_init, netif, mainloop)
  // and Thread credential provisioning during Matter commissioning.
  // For Thread, mDNS is not used - CHIP uses SRP/DNS-SD via the border router.
  {
    esp_event_loop_create_default();  // required before OpenThread init
    esp_openthread_platform_config_t ot_config = {
        .radio_config =
            {
                .radio_mode = RADIO_MODE_NATIVE,
                .radio_uart_config = {},
            },
        .host_config = {},
        .port_config =
            {
                .storage_partition_name = "nvs",
                .netif_queue_size = 10,
                .task_queue_size = 10,
            },
    };
    set_openthread_platform_config(&ot_config);
    ESP_LOGI(TAG, "Thread mode: configured OpenThread platform for CHIP");
  }
#endif

  // Create the Matter node (root node on endpoint 0)
  esp_matter::node::config_t node_config;
  this->node_ = esp_matter::node::create(&node_config, attribute_update_cb_, identification_cb_);
  if (this->node_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create Matter node");
    this->mark_failed();
    return;
  }

  // Create Matter endpoints for all registered ESPHome entities
  this->create_endpoints_();

  // Use the example DAC provider for development
  // In production, this should be replaced with proper device attestation credentials
  chip::Credentials::SetDeviceAttestationCredentialsProvider(
      chip::Credentials::Examples::GetExampleDACProvider());

  // Start the Matter stack
  err = esp_matter::start(event_cb_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start Matter: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  this->matter_started_ = true;

  // Also set via the CommissionableDataProvider API (updates in-memory state)
  chip::DeviceLayer::GetCommissionableDataProvider()->SetSetupDiscriminator(this->discriminator_);
  chip::DeviceLayer::GetCommissionableDataProvider()->SetSetupPasscode(this->passcode_);

  // Generate and log the QR code setup payload for pairing
  this->log_qr_code_();

  ESP_LOGI(TAG, "Matter started successfully");
}

void MatterComponent::log_qr_code_() {
  // Build a Matter setup payload and generate the QR code string
  chip::SetupPayload payload;
  payload.version = 0;
  payload.vendorID = this->vendor_id_;
  payload.productID = this->product_id_;
  payload.commissioningFlow = chip::CommissioningFlow::kStandard;
  payload.discriminator.SetLongValue(this->discriminator_);
  payload.setUpPINCode = this->passcode_;
  // BLE for commissioning discovery
  payload.rendezvousInformation.SetValue(chip::RendezvousInformationFlag::kBLE);

  // Generate QR code data string (starts with "MT:")
  chip::QRCodeSetupPayloadGenerator qr_generator(payload);
  std::string qr_code;
  CHIP_ERROR chip_err = qr_generator.payloadBase38Representation(qr_code);
  if (chip_err == CHIP_NO_ERROR) {
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "Matter QR Code: %s", qr_code.c_str());
    ESP_LOGI(TAG, "Manual pairing code: discriminator=%u, passcode=%" PRIu32, this->discriminator_, this->passcode_);
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "Scan the QR code with your Matter controller app (Apple Home, Google Home, etc.)");
  } else {
    ESP_LOGW(TAG, "Failed to generate QR code payload");
    ESP_LOGI(TAG, "Manual pairing: discriminator=%u, passcode=%" PRIu32, this->discriminator_, this->passcode_);
  }
}

void MatterComponent::loop() {
  // The Matter stack runs in its own task, so no per-loop work is needed here.
  // Entity state changes are handled via Controller callbacks which push updates
  // to Matter attributes directly.
}

void MatterComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Matter:");
  ESP_LOGCONFIG(TAG, "  Vendor ID: 0x%04X", this->vendor_id_);
  ESP_LOGCONFIG(TAG, "  Product ID: 0x%04X", this->product_id_);
  ESP_LOGCONFIG(TAG, "  Discriminator: %u", this->discriminator_);
  if (this->product_name_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Product Name: %s", this->product_name_);
  }
  ESP_LOGCONFIG(TAG, "  Endpoints created: %zu", this->endpoint_types_.size());

  for (const auto &pair : this->endpoint_types_) {
    const char *type_str = "unknown";
    switch (pair.second) {
      case EndpointType::FAN:
        type_str = "Fan";
        break;
      case EndpointType::ON_OFF:
        type_str = "OnOff";
        break;
      case EndpointType::CONTACT_SENSOR:
        type_str = "ContactSensor";
        break;
      case EndpointType::TEMPERATURE_SENSOR:
        type_str = "TemperatureSensor";
        break;
      case EndpointType::HUMIDITY_SENSOR:
        type_str = "HumiditySensor";
        break;
      case EndpointType::PM25_SENSOR:
        type_str = "PM2.5Sensor";
        break;
      case EndpointType::MODE_SELECT:
        type_str = "ModeSelect";
        break;
      case EndpointType::DIMMABLE:
        type_str = "Dimmable";
        break;
    }
    ESP_LOGCONFIG(TAG, "  Endpoint %u: %s", pair.first, type_str);
  }
}

// ---- Entity endpoint creation ----

void MatterComponent::create_endpoints_() {
  if (this->node_ == nullptr) {
    return;
  }

#ifdef USE_FAN
  for (auto *fan_entity : App.get_fans()) {
    if (fan_entity->is_internal())
      continue;
    uint16_t ep_id = this->create_fan_endpoint_(fan_entity->get_name().c_str());
    if (ep_id > 0) {
      this->entity_endpoint_map_[fan_entity] = ep_id;
      this->endpoint_entity_map_[ep_id] = fan_entity;
      ESP_LOGI(TAG, "Created fan endpoint %u for '%s'", ep_id, fan_entity->get_name().c_str());
    }
  }
#endif

#ifdef USE_SWITCH
  for (auto *sw : App.get_switches()) {
    if (sw->is_internal())
      continue;
    uint16_t ep_id = this->create_on_off_endpoint_(sw->get_name().c_str());
    if (ep_id > 0) {
      this->entity_endpoint_map_[sw] = ep_id;
      this->endpoint_entity_map_[ep_id] = sw;
      ESP_LOGI(TAG, "Created on/off endpoint %u for '%s'", ep_id, sw->get_name().c_str());
    }
  }
#endif

#ifdef USE_BINARY_SENSOR
  for (auto *bs : App.get_binary_sensors()) {
    if (bs->is_internal())
      continue;
    uint16_t ep_id = this->create_contact_sensor_endpoint_(bs->get_name().c_str());
    if (ep_id > 0) {
      this->entity_endpoint_map_[bs] = ep_id;
      this->endpoint_entity_map_[ep_id] = bs;
      ESP_LOGI(TAG, "Created contact sensor endpoint %u for '%s'", ep_id, bs->get_name().c_str());
    }
  }
#endif

#ifdef USE_SENSOR
  for (auto *sensor_entity : App.get_sensors()) {
    if (sensor_entity->is_internal())
      continue;
    // Use get_device_class_ref() to avoid deprecated API and string copies
    auto device_class_ref = sensor_entity->get_device_class_ref();

    uint16_t ep_id = 0;
    if (device_class_ref == "temperature") {
      ep_id = this->create_temperature_sensor_endpoint_(sensor_entity->get_name().c_str());
    } else if (device_class_ref == "humidity") {
      ep_id = this->create_humidity_sensor_endpoint_(sensor_entity->get_name().c_str());
    } else if (device_class_ref == "pm25") {
      ep_id = this->create_pm25_sensor_endpoint_(sensor_entity->get_name().c_str());
    } else {
      // Skip sensors without a mappable device class
      ESP_LOGD(TAG, "Skipping sensor '%s' (no Matter mapping for device_class)",
               sensor_entity->get_name().c_str());
      continue;
    }

    if (ep_id > 0) {
      this->entity_endpoint_map_[sensor_entity] = ep_id;
      this->endpoint_entity_map_[ep_id] = sensor_entity;
      ESP_LOGI(TAG, "Created sensor endpoint %u for '%s'", ep_id, sensor_entity->get_name().c_str());
    }
  }
#endif

#ifdef USE_SELECT
  for (auto *sel : App.get_selects()) {
    if (sel->is_internal())
      continue;
    const auto &options = sel->traits.get_options();
    std::vector<std::string> option_strings(options.begin(), options.end());
    uint16_t ep_id = this->create_mode_select_endpoint_(sel->get_name().c_str(), option_strings);
    if (ep_id > 0) {
      this->entity_endpoint_map_[sel] = ep_id;
      this->endpoint_entity_map_[ep_id] = sel;
      ESP_LOGI(TAG, "Created mode select endpoint %u for '%s'", ep_id, sel->get_name().c_str());
    }
  }
#endif

#ifdef USE_NUMBER
  for (auto *num : App.get_numbers()) {
    if (num->is_internal())
      continue;
    uint16_t ep_id = this->create_dimmable_endpoint_(num->get_name().c_str());
    if (ep_id > 0) {
      this->entity_endpoint_map_[num] = ep_id;
      this->endpoint_entity_map_[ep_id] = num;
      ESP_LOGI(TAG, "Created dimmable endpoint %u for '%s'", ep_id, num->get_name().c_str());
    }
  }
#endif
}

uint16_t MatterComponent::create_fan_endpoint_(const std::string &name) {
  esp_matter::endpoint::fan::config_t fan_config;
  esp_matter::endpoint_t *ep =
      esp_matter::endpoint::fan::create(this->node_, &fan_config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
  if (ep == nullptr) {
    ESP_LOGE(TAG, "Failed to create fan endpoint for '%s'", name.c_str());
    return 0;
  }
  uint16_t ep_id = esp_matter::endpoint::get_id(ep);
  this->endpoint_types_[ep_id] = EndpointType::FAN;
  return ep_id;
}

uint16_t MatterComponent::create_on_off_endpoint_(const std::string &name) {
  esp_matter::endpoint::on_off_plugin_unit::config_t on_off_config;
  esp_matter::endpoint_t *ep =
      esp_matter::endpoint::on_off_plugin_unit::create(this->node_, &on_off_config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
  if (ep == nullptr) {
    ESP_LOGE(TAG, "Failed to create on/off endpoint for '%s'", name.c_str());
    return 0;
  }
  uint16_t ep_id = esp_matter::endpoint::get_id(ep);
  this->endpoint_types_[ep_id] = EndpointType::ON_OFF;
  return ep_id;
}

uint16_t MatterComponent::create_contact_sensor_endpoint_(const std::string &name) {
  esp_matter::endpoint::contact_sensor::config_t contact_config;
  esp_matter::endpoint_t *ep =
      esp_matter::endpoint::contact_sensor::create(this->node_, &contact_config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
  if (ep == nullptr) {
    ESP_LOGE(TAG, "Failed to create contact sensor endpoint for '%s'", name.c_str());
    return 0;
  }
  uint16_t ep_id = esp_matter::endpoint::get_id(ep);
  this->endpoint_types_[ep_id] = EndpointType::CONTACT_SENSOR;
  return ep_id;
}

uint16_t MatterComponent::create_temperature_sensor_endpoint_(const std::string &name) {
  esp_matter::endpoint::temperature_sensor::config_t temp_config;
  esp_matter::endpoint_t *ep =
      esp_matter::endpoint::temperature_sensor::create(this->node_, &temp_config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
  if (ep == nullptr) {
    ESP_LOGE(TAG, "Failed to create temperature sensor endpoint for '%s'", name.c_str());
    return 0;
  }
  uint16_t ep_id = esp_matter::endpoint::get_id(ep);
  this->endpoint_types_[ep_id] = EndpointType::TEMPERATURE_SENSOR;
  return ep_id;
}

uint16_t MatterComponent::create_humidity_sensor_endpoint_(const std::string &name) {
  esp_matter::endpoint::humidity_sensor::config_t humidity_config;
  esp_matter::endpoint_t *ep =
      esp_matter::endpoint::humidity_sensor::create(this->node_, &humidity_config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
  if (ep == nullptr) {
    ESP_LOGE(TAG, "Failed to create humidity sensor endpoint for '%s'", name.c_str());
    return 0;
  }
  uint16_t ep_id = esp_matter::endpoint::get_id(ep);
  this->endpoint_types_[ep_id] = EndpointType::HUMIDITY_SENSOR;
  return ep_id;
}

uint16_t MatterComponent::create_pm25_sensor_endpoint_(const std::string &name) {
  // Create an air quality sensor endpoint
  esp_matter::endpoint::air_quality_sensor::config_t aq_config;
  esp_matter::endpoint_t *ep =
      esp_matter::endpoint::air_quality_sensor::create(this->node_, &aq_config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
  if (ep == nullptr) {
    ESP_LOGE(TAG, "Failed to create air quality sensor endpoint for '%s'", name.c_str());
    return 0;
  }

  // Add PM2.5 Concentration Measurement cluster (0x042A) to the endpoint
  esp_matter::cluster_t *pm25_cluster = esp_matter::cluster::create(
      ep, Pm25ConcentrationMeasurement::Id, esp_matter::CLUSTER_FLAG_SERVER);
  if (pm25_cluster != nullptr) {
    // MeasuredValue attribute (nullable float - stored as nullable single in Matter)
    esp_matter_attr_val_t measured_val = esp_matter_nullable_float(0.0f);
    esp_matter::attribute::create(pm25_cluster, 0x00000000,  // MeasuredValue
                                  esp_matter::ATTRIBUTE_FLAG_NULLABLE, measured_val);
    // MinMeasuredValue
    esp_matter_attr_val_t min_val = esp_matter_nullable_float(0.0f);
    esp_matter::attribute::create(pm25_cluster, 0x00000001,  // MinMeasuredValue
                                  esp_matter::ATTRIBUTE_FLAG_NULLABLE, min_val);
    // MaxMeasuredValue
    esp_matter_attr_val_t max_val = esp_matter_nullable_float(1000.0f);
    esp_matter::attribute::create(pm25_cluster, 0x00000002,  // MaxMeasuredValue
                                  esp_matter::ATTRIBUTE_FLAG_NULLABLE, max_val);
    // MeasurementUnit attribute (0x00000008) - Matter MeasurementUnitEnum:
    //   PPM=0, PPB=1, PPT=2, MGM3=3, UGM3=4, BQM3=5, NGPM3=6
    esp_matter_attr_val_t unit_val = esp_matter_enum8(4);  // UGM3 (µg/m³) for PM2.5
    esp_matter::attribute::create(pm25_cluster, 0x00000008,  // MeasurementUnit
                                  esp_matter::ATTRIBUTE_FLAG_NONE, unit_val);
  }

  uint16_t ep_id = esp_matter::endpoint::get_id(ep);
  this->endpoint_types_[ep_id] = EndpointType::PM25_SENSOR;
  return ep_id;
}

uint16_t MatterComponent::create_mode_select_endpoint_(const std::string &name,
                                                        const std::vector<std::string> &options) {
  esp_matter::endpoint::mode_select_device::config_t ms_config;
  // Set the description for the mode select
  strncpy(ms_config.mode_select.mode_select_description, name.c_str(),
          sizeof(ms_config.mode_select.mode_select_description) - 1);
  ms_config.mode_select.mode_select_description[sizeof(ms_config.mode_select.mode_select_description) - 1] = '\0';
  ms_config.mode_select.current_mode = 0;

  esp_matter::endpoint_t *ep =
      esp_matter::endpoint::mode_select_device::create(this->node_, &ms_config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
  if (ep == nullptr) {
    ESP_LOGE(TAG, "Failed to create mode select endpoint for '%s'", name.c_str());
    return 0;
  }

  // Add SupportedModes entries
  // The ModeSelect cluster SupportedModes is a list attribute.
  // For now we configure the mode count in the description and update CurrentMode dynamically.
  // Note: SupportedModes list population requires TLV encoding which is complex;
  // the mode_select cluster created by esp_matter handles the basic structure.

  uint16_t ep_id = esp_matter::endpoint::get_id(ep);
  this->endpoint_types_[ep_id] = EndpointType::MODE_SELECT;
  return ep_id;
}

uint16_t MatterComponent::create_dimmable_endpoint_(const std::string &name) {
  esp_matter::endpoint::dimmable_light::config_t dim_config;
  dim_config.level_control.current_level = nullable<uint8_t>(128);  // Start at 50%
  dim_config.level_control.on_level = nullable<uint8_t>(128);

  esp_matter::endpoint_t *ep =
      esp_matter::endpoint::dimmable_light::create(this->node_, &dim_config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
  if (ep == nullptr) {
    ESP_LOGE(TAG, "Failed to create dimmable endpoint for '%s'", name.c_str());
    return 0;
  }
  uint16_t ep_id = esp_matter::endpoint::get_id(ep);
  this->endpoint_types_[ep_id] = EndpointType::DIMMABLE;
  return ep_id;
}

// ---- Controller callbacks: ESPHome state -> Matter attributes ----

#ifdef USE_FAN
void MatterComponent::on_fan_update(fan::Fan *obj) {
  auto it = this->entity_endpoint_map_.find(obj);
  if (it == this->entity_endpoint_map_.end())
    return;
  uint16_t ep_id = it->second;

  // Update OnOff cluster
  esp_matter_attr_val_t on_off_val = esp_matter_bool(obj->state);
  esp_matter::attribute::update(ep_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &on_off_val);

  // Update FanControl cluster - PercentSetting
  // ESPHome fan speed is 0-100 (matching the speed_count), map to Matter 0-100
  if (obj->get_traits().supports_speed()) {
    int speed_count = obj->get_traits().supported_speed_count();
    uint8_t percent = 0;
    if (speed_count > 0 && obj->speed > 0) {
      percent = static_cast<uint8_t>((obj->speed * 100) / speed_count);
    }
    esp_matter_attr_val_t speed_val = esp_matter_nullable_uint8(percent);
    esp_matter::attribute::update(ep_id, FanControl::Id, FanControl::Attributes::PercentSetting::Id, &speed_val);
  }
}
#endif

#ifdef USE_SWITCH
void MatterComponent::on_switch_update(switch_::Switch *obj) {
  auto it = this->entity_endpoint_map_.find(obj);
  if (it == this->entity_endpoint_map_.end())
    return;
  uint16_t ep_id = it->second;

  esp_matter_attr_val_t val = esp_matter_bool(obj->state);
  esp_matter::attribute::update(ep_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
}
#endif

#ifdef USE_BINARY_SENSOR
void MatterComponent::on_binary_sensor_update(binary_sensor::BinarySensor *obj) {
  auto it = this->entity_endpoint_map_.find(obj);
  if (it == this->entity_endpoint_map_.end())
    return;
  uint16_t ep_id = it->second;

  // BooleanState cluster: StateValue attribute
  esp_matter_attr_val_t val = esp_matter_bool(obj->state);
  esp_matter::attribute::update(ep_id, BooleanState::Id, BooleanState::Attributes::StateValue::Id, &val);
}
#endif

#ifdef USE_SENSOR
void MatterComponent::on_sensor_update(sensor::Sensor *obj) {
  auto it = this->entity_endpoint_map_.find(obj);
  if (it == this->entity_endpoint_map_.end())
    return;
  uint16_t ep_id = it->second;

  auto type_it = this->endpoint_types_.find(ep_id);
  if (type_it == this->endpoint_types_.end())
    return;

  if (std::isnan(obj->state))
    return;

  switch (type_it->second) {
    case EndpointType::TEMPERATURE_SENSOR: {
      // Matter uses temperature in 0.01 degrees Celsius
      int16_t temp_val = static_cast<int16_t>(obj->state * 100);
      esp_matter_attr_val_t val = esp_matter_nullable_int16(temp_val);
      esp_matter::attribute::update(ep_id, TemperatureMeasurement::Id,
                                    TemperatureMeasurement::Attributes::MeasuredValue::Id, &val);
      break;
    }
    case EndpointType::HUMIDITY_SENSOR: {
      // Matter uses humidity in 0.01 percent
      uint16_t humidity_val = static_cast<uint16_t>(obj->state * 100);
      esp_matter_attr_val_t val = esp_matter_nullable_uint16(humidity_val);
      esp_matter::attribute::update(ep_id, RelativeHumidityMeasurement::Id,
                                    RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, &val);
      break;
    }
    case EndpointType::PM25_SENSOR: {
      // PM2.5 Concentration Measurement: MeasuredValue is a float (µg/m³)
      esp_matter_attr_val_t val = esp_matter_nullable_float(obj->state);
      esp_matter::attribute::update(ep_id, Pm25ConcentrationMeasurement::Id,
                                    Pm25ConcentrationMeasurement::Attributes::MeasuredValue::Id, &val);
      break;
    }
    default:
      break;
  }
}
#endif

#ifdef USE_SELECT
void MatterComponent::on_select_update(select::Select *obj) {
  auto it = this->entity_endpoint_map_.find(obj);
  if (it == this->entity_endpoint_map_.end())
    return;
  uint16_t ep_id = it->second;

  // Map the current selected option index to ModeSelect::CurrentMode
  const auto &state = obj->current_option();
  const auto &options = obj->traits.get_options();
  uint8_t mode = 0;
  for (size_t i = 0; i < options.size(); i++) {
    if (options[i] == state) {
      mode = static_cast<uint8_t>(i);
      break;
    }
  }
  esp_matter_attr_val_t val = esp_matter_uint8(mode);
  esp_matter::attribute::update(ep_id, ModeSelect::Id, ModeSelect::Attributes::CurrentMode::Id, &val);
}
#endif

#ifdef USE_NUMBER
void MatterComponent::on_number_update(number::Number *obj) {
  auto it = this->entity_endpoint_map_.find(obj);
  if (it == this->entity_endpoint_map_.end())
    return;
  uint16_t ep_id = it->second;

  if (std::isnan(obj->state))
    return;

  // Map number value to LevelControl::CurrentLevel (0-254)
  // Scale from the number's min/max range to 0-254
  float min_val = obj->traits.get_min_value();
  float max_val = obj->traits.get_max_value();
  float range = max_val - min_val;
  uint8_t level = 0;
  if (range > 0) {
    level = static_cast<uint8_t>(((obj->state - min_val) / range) * 254.0f);
  }
  esp_matter_attr_val_t val = esp_matter_nullable_uint8(level);
  esp_matter::attribute::update(ep_id, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, &val);

  // Also update OnOff state based on whether the number is at minimum
  esp_matter_attr_val_t on_off_val = esp_matter_bool(obj->state > min_val);
  esp_matter::attribute::update(ep_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &on_off_val);
}
#endif

// ---- Matter SDK callbacks ----

esp_err_t MatterComponent::attribute_update_cb_(esp_matter::attribute::callback_type_t type, uint16_t endpoint_id,
                                                uint32_t cluster_id, uint32_t attribute_id,
                                                esp_matter_attr_val_t *val, void *priv_data) {
  if (global_matter == nullptr)
    return ESP_OK;

  if (type == esp_matter::attribute::PRE_UPDATE) {
    return global_matter->handle_attribute_update_(endpoint_id, cluster_id, attribute_id, val);
  }
  return ESP_OK;
}

esp_err_t MatterComponent::identification_cb_(esp_matter::identification::callback_type_t type, uint16_t endpoint_id,
                                              uint8_t effect_id, uint8_t effect_variant, void *priv_data) {
  ESP_LOGI(TAG, "Identify callback: type=%u, endpoint=%u, effect=%u", type, endpoint_id, effect_id);
  return ESP_OK;
}

void MatterComponent::event_cb_(const ChipDeviceEvent *event, intptr_t arg) {
  switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
      ESP_LOGI(TAG, "Commissioning complete");
      break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
      ESP_LOGI(TAG, "Commissioning session started");
      break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
      ESP_LOGI(TAG, "Commissioning window opened");
      break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
      ESP_LOGI(TAG, "Commissioning window closed");
      break;
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved: {
      ESP_LOGI(TAG, "Fabric removed");
      // Re-open commissioning window if all fabrics removed
      if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
        auto &commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
        if (!commissionMgr.IsCommissioningWindowOpen()) {
          constexpr auto kTimeout = chip::System::Clock::Seconds16(300);
          commissionMgr.OpenBasicCommissioningWindow(kTimeout,
                                                     chip::CommissioningWindowAdvertisement::kDnssdOnly);
        }
      }
      break;
    }
    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
      ESP_LOGI(TAG, "BLE deinitialized (memory reclaimed)");
      break;
    default:
      break;
  }
}

// ---- Matter attribute write -> ESPHome entity command ----

esp_err_t MatterComponent::handle_attribute_update_(uint16_t endpoint_id, uint32_t cluster_id,
                                                    uint32_t attribute_id, esp_matter_attr_val_t *val) {
  auto entity_it = this->endpoint_entity_map_.find(endpoint_id);
  if (entity_it == this->endpoint_entity_map_.end())
    return ESP_OK;

  auto type_it = this->endpoint_types_.find(endpoint_id);
  if (type_it == this->endpoint_types_.end())
    return ESP_OK;

  switch (type_it->second) {
#ifdef USE_FAN
    case EndpointType::FAN: {
      auto *fan_entity = static_cast<fan::Fan *>(entity_it->second);
      if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        auto call = fan_entity->make_call();
        call.set_state(val->val.b);
        call.perform();
      } else if (cluster_id == FanControl::Id &&
                 attribute_id == FanControl::Attributes::PercentSetting::Id) {
        auto call = fan_entity->make_call();
        int speed_count = fan_entity->get_traits().supported_speed_count();
        if (speed_count > 0) {
          int speed = (val->val.u8 * speed_count + 99) / 100;
          call.set_speed(speed);
        }
        call.perform();
      }
      break;
    }
#endif

#ifdef USE_SWITCH
    case EndpointType::ON_OFF: {
      auto *sw = static_cast<switch_::Switch *>(entity_it->second);
      if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        if (val->val.b) {
          sw->turn_on();
        } else {
          sw->turn_off();
        }
      }
      break;
    }
#endif

    case EndpointType::CONTACT_SENSOR:
    case EndpointType::TEMPERATURE_SENSOR:
    case EndpointType::HUMIDITY_SENSOR:
    case EndpointType::PM25_SENSOR:
      // Sensors are read-only, no commands to dispatch
      break;

#ifdef USE_SELECT
    case EndpointType::MODE_SELECT: {
      auto *sel = static_cast<select::Select *>(entity_it->second);
      if (cluster_id == ModeSelect::Id && attribute_id == ModeSelect::Attributes::CurrentMode::Id) {
        uint8_t mode = val->val.u8;
        const auto &options = sel->traits.get_options();
        if (mode < options.size()) {
          auto call = sel->make_call();
          call.set_option(options[mode]);
          call.perform();
        }
      }
      break;
    }
#endif

#ifdef USE_NUMBER
    case EndpointType::DIMMABLE: {
      auto *num = static_cast<number::Number *>(entity_it->second);
      if (cluster_id == LevelControl::Id && attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
        // Scale from 0-254 back to number's min/max range
        float min_val = num->traits.get_min_value();
        float max_val = num->traits.get_max_value();
        float range = max_val - min_val;
        float value = min_val + (static_cast<float>(val->val.u8) / 254.0f) * range;
        auto call = num->make_call();
        call.set_value(value);
        call.perform();
      } else if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        // On/Off maps to min (off) or last value (on)
        auto call = num->make_call();
        if (val->val.b) {
          call.set_value(num->traits.get_max_value());
        } else {
          call.set_value(num->traits.get_min_value());
        }
        call.perform();
      }
      break;
    }
#endif
  }

  return ESP_OK;
}

}  // namespace esphome::matter
#endif  // USE_MATTER
