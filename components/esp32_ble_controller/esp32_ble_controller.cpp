#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLESecurity.h>

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <esp_bt_main.h>
#include <esp32-hal-bt.h>

#include "esp32_ble_controller.h"

#include "ble_maintenance_handler.h"
#include "ble_utils.h"
#include "ble_command.h"
#include "automation.h"
#include "ble_component_handler_factory.h"

namespace esphome {
namespace esp32_ble_controller {

static const char *TAG = "esp32_ble_controller";


boolean static_passkey = false;
char *static_passkey_value[6]; 

ESP32BLEController::ESP32BLEController() : maintenance_handler(new BLEMaintenanceHandler()) {}

/// pre-setup configuration ///////////////////////////////////////////////////////////////////////////////////////////////////////////////

void ESP32BLEController::register_component(EntityBase* component, const string& serviceUUID, const string& characteristic_UUID, bool use_BLE2902) {
  BLECharacteristicInfoForHandler info;
  info.service_UUID = serviceUUID;
  info.characteristic_UUID = characteristic_UUID;
  info.use_BLE2902 = use_BLE2902;

  info_for_component[component->get_object_id()] = info;
}

void ESP32BLEController::ESP32BLEController::register_command(const string& name, const string& description, BLEControllerCustomCommandExecutionTrigger* trigger) {
  maintenance_handler->add_command(new BLECustomCommand(name, description, trigger));
}

const vector<BLECommand*>& ESP32BLEController::get_commands() const {
  return maintenance_handler->get_commands();
}

void ESP32BLEController::add_on_show_pass_key_callback(std::function<void(string)>&& trigger_function) {
  can_show_pass_key = true;
  on_show_pass_key_callbacks.add(std::move(trigger_function));
}

void ESP32BLEController::add_on_authentication_complete_callback(std::function<void(bool)>&& trigger_function) {
  on_authentication_complete_callbacks.add(std::move(trigger_function));
}

void ESP32BLEController::add_on_connected_callback(std::function<void()>&& trigger_function) {
  on_connected_callbacks.add(std::move(trigger_function));
}

void ESP32BLEController::add_on_disconnected_callback(std::function<void()>&& trigger_function) {
  on_disconnected_callbacks.add(std::move(trigger_function));
}

BLEMaintenanceMode set_feature(BLEMaintenanceMode current_mode, BLEMaintenanceMode feature, bool is_set) {
  uint8_t new_mode = static_cast<uint8_t>(current_mode) & (~static_cast<uint8_t>(feature));
  if (is_set) {
    new_mode |= static_cast<uint8_t>(feature);
  }
  return static_cast<BLEMaintenanceMode>(new_mode);
}

void ESP32BLEController::set_maintenance_service_exposed_after_flash(bool exposed) {
  initial_ble_mode_after_flashing = set_feature(initial_ble_mode_after_flashing, BLEMaintenanceMode::MAINTENANCE_SERVICE, exposed);    
}

void ESP32BLEController::set_security_enabled(bool enabled) {
  set_security_mode(BLESecurityMode::SECURE);
}

void ESP32BLEController::set_static_passkey(char passkey[6]) {
  static_passkey = true;
  static_passkey_value = passkey;
}


/// setup ///////////////////////////////////////////////////////////////////////////////////////////////////////////////

void ESP32BLEController::setup() {
  ESP_LOGCONFIG(TAG, "Setting up BLE controller ...");

  initialize_ble_mode();

  if (ble_mode == BLEMaintenanceMode::NONE) {
    ESP_LOGCONFIG(TAG, "BLE inactive");
    return;
  }

  #ifdef USE_WIFI
  wifi_configuration_handler.setup();
  #endif

  if (!setup_ble()) {
    return;
  }

  if (global_ble_controller == nullptr) {
    global_ble_controller = this;
  } else {
    ESP_LOGE(TAG, "Already have an instance of the BLE controller");
  }

  // Create the BLE Device
  BLEDevice::init(App.get_name());

  configure_ble_security();

  setup_ble_server_and_services();

  // Start advertising
  // BLEAdvertising* advertising = BLEDevice::getAdvertising();
  // advertising->setMinInterval(0x800); // suggested default: 1.28s
  // advertising->setMaxInterval(0x800);
  // advertising->setMinPreferred(80); // = 100 ms, see https://www.novelbits.io/ble-connection-intervals/, https://www.novelbits.io/bluetooth-low-energy-advertisements-part-1/
  // advertising->setMaxPreferred(800); // = 1000 ms
  BLEDevice::startAdvertising();
}

bool ESP32BLEController::setup_ble() {
  if (btStarted()) {
    ESP_LOGI(TAG, "BLE already started");
    return true;
  }

  ESP_LOGI(TAG, "  Setting up BLE ...");

  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

  // Initialize the bluetooth controller with the default configuration
  if (!btStart()) {
    ESP_LOGE(TAG, "btStart failed: %d", esp_bt_controller_get_status());
    mark_failed();
    return false;
  }

  esp_err_t err = esp_bluedroid_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_bluedroid_init failed: %d", err);
    mark_failed();
    return false;
  }

  err = esp_bluedroid_enable();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_bluedroid_enable failed: %d", err);
    mark_failed();
    return false;
  }

  return true;
}

void ESP32BLEController::setup_ble_server_and_services() {
  ble_server = BLEDevice::createServer();
  ble_server->setCallbacks(this);

  if (get_maintenance_service_exposed()) {
    maintenance_handler->setup(ble_server);
  }

  if (get_component_services_exposed()) {
    setup_ble_services_for_components();
  }
}

void ESP32BLEController::setup_ble_services_for_components() {
#ifdef USE_BINARY_SENSOR
  setup_ble_services_for_components(App.get_binary_sensors(), BLEComponentHandlerFactory::create_binary_sensor_handler);
#endif
#ifdef USE_COVER
  //setup_ble_services_for_components(App.get_covers());
#endif
#ifdef USE_FAN
  setup_ble_services_for_components(App.get_fans(), BLEComponentHandlerFactory::create_fan_handler);
#endif
#ifdef USE_LIGHT
  //setup_ble_services_for_components(App.get_lights());
#endif
#ifdef USE_SENSOR
  setup_ble_services_for_components(App.get_sensors(), BLEComponentHandlerFactory::create_sensor_handler);
#endif
#ifdef USE_SWITCH
  setup_ble_services_for_components(App.get_switches(), BLEComponentHandlerFactory::create_switch_handler);
#endif
#ifdef USE_TEXT_SENSOR
  setup_ble_services_for_components(App.get_text_sensors(), BLEComponentHandlerFactory::create_text_sensor_handler);
#endif
#ifdef USE_CLIMATE
  //setup_ble_services_for_components(App.get_climates());
#endif

  for (auto const& entry : handler_for_component) {
    auto* handler = entry.second;
    handler->setup(ble_server);
  }

  register_state_change_callbacks_and_send_initial_states();
}

template <typename C> 
void ESP32BLEController::setup_ble_services_for_components(const vector<C*>& components, BLEComponentHandlerBase* (*handler_creator)(C*, const BLECharacteristicInfoForHandler&)) {
  for (C* component: components) {
    setup_ble_service_for_component(component, handler_creator);
  }
}

template <typename C> 
void ESP32BLEController::setup_ble_service_for_component(C* component, BLEComponentHandlerBase* (*handler_creator)(C*, const BLECharacteristicInfoForHandler&)) {
  static_assert(std::is_base_of<EntityBase, C>::value, "EntityBase subclasses expected");

  auto object_id = component->get_object_id();
  if (info_for_component.count(object_id)) {
    auto info = info_for_component[object_id];
    handler_for_component[object_id] = handler_creator(component, info);
  }
}

void ESP32BLEController::register_state_change_callbacks_and_send_initial_states() {
#ifdef USE_BINARY_SENSOR
  for (auto *obj : App.get_binary_sensors()) {
    if (info_for_component.count(obj->get_object_id())) {
      obj->add_on_state_callback([this, obj](bool state) { this->on_binary_sensor_update(obj, state); });
      if (obj->has_state())
        update_component_state(obj, obj->state);
    }
  }
#endif
#ifdef USE_CLIMATE
  // for (auto *obj : App.get_climates()) {
  //   if (info_for_component.count(obj->get_object_id()))
  //     obj->add_on_state_callback([this, obj]() { this->on_climate_update(obj); });
  // }
#endif
#ifdef USE_COVER
  // for (auto *obj : App.get_covers()) {
  //   if (info_for_component.count(obj->get_object_id()))
  //     obj->add_on_state_callback([this, obj]() { this->on_cover_update(obj); });
  // }
#endif
#ifdef USE_FAN
  for (auto *obj : App.get_fans()) {
    if (info_for_component.count(obj->get_object_id())) {
      obj->add_on_state_callback([this, obj]() { this->on_fan_update(obj); });
      update_component_state(obj, obj->state);
    }
  }
#endif
#ifdef USE_LIGHT
  // for (auto *obj : App.get_lights()) {
  //   if (info_for_component.count(obj->get_object_id()))
  //     obj->add_new_remote_values_callback([this, obj]() { this->on_light_update(obj); });
  // }
#endif
#ifdef USE_SENSOR
  for (auto *obj : App.get_sensors()) {
    if (info_for_component.count(obj->get_object_id())) {
      obj->add_on_state_callback([this, obj](float state) { this->on_sensor_update(obj, state); });
      if (obj->has_state())
        update_component_state(obj, obj->state);
    }
  }
#endif
#ifdef USE_SWITCH
  for (auto *obj : App.get_switches()) {
    if (info_for_component.count(obj->get_object_id())) {
      obj->add_on_state_callback([this, obj](bool state) { this->on_switch_update(obj, state); });
      update_component_state(obj, obj->state);
    }
  }
#endif
#ifdef USE_TEXT_SENSOR
  for (auto *obj : App.get_text_sensors()) {
    if (info_for_component.count(obj->get_object_id())) {
      obj->add_on_state_callback([this, obj](std::string state) { this->on_text_sensor_update(obj, state); });
      if (obj->has_state())
        update_component_state(obj, obj->state);
    }
  }
#endif
}

void ESP32BLEController::initialize_ble_mode() {
  // Note: We include the compilation time to force a reset after flashing new firmware
  ble_mode_preference = global_preferences->make_preference<uint8_t>(fnv1_hash("ble-mode#" + App.get_compilation_time()));

  if (!ble_mode_preference.load(&ble_mode)) {
    ble_mode = initial_ble_mode_after_flashing;
  }

  ESP_LOGCONFIG(TAG, "BLE mode: %d", static_cast<uint8_t>(ble_mode));
}

void ESP32BLEController::switch_ble_mode(BLEMaintenanceMode newMode) {
  if (ble_mode != newMode) {
    ESP_LOGI(TAG, "Switching BLE mode to %d and rebooting", static_cast<uint8_t>(newMode));

    ble_mode = newMode;
    ble_mode_preference.save(&ble_mode);

    App.safe_reboot();
  }
}

void ESP32BLEController::switch_maintenance_service_exposed(bool exposed) {
  switch_ble_mode(set_feature(ble_mode, BLEMaintenanceMode::MAINTENANCE_SERVICE, exposed));
}

void ESP32BLEController::switch_component_services_exposed(bool exposed) {
  switch_ble_mode(set_feature(ble_mode, BLEMaintenanceMode::COMPONENT_SERVICES, exposed));
}

void ESP32BLEController::dump_config() {
  if (ble_mode == BLEMaintenanceMode::NONE) {
    return;
  }
  
  ESP_LOGCONFIG(TAG, "Bluetooth Low Energy Controller:");
  ESP_LOGCONFIG(TAG, "  BLE device address: %s", BLEDevice::getAddress().toString().c_str());
  ESP_LOGCONFIG(TAG, "  BLE mode: %d", (uint8_t) ble_mode);

  if (get_security_mode() != BLESecurityMode::NONE) {
    if (get_security_mode() == BLESecurityMode::BOND) {
      ESP_LOGCONFIG(TAG, "  only bonding enabled, no real security");
    } else {
      ESP_LOGCONFIG(TAG, "  security enabled (secure connections, MITM protection)");
    }

    vector<string> bonded_devices = get_bonded_devices();
    if (bonded_devices.empty()) {
      ESP_LOGCONFIG(TAG, "  no bonded BLE devices");
    } else {
      ESP_LOGCONFIG(TAG, "  bonded BLE devices (%d):", bonded_devices.size());
      int i = 0;
      for (const auto& bd_address : bonded_devices) {
        ESP_LOGCONFIG(TAG, "    %d) BD address %s", ++i, bd_address.c_str());
      }
    }
  } else {
    ESP_LOGCONFIG(TAG, "  security disabled");
  }
}

/// run ///////////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifdef USE_WIFI
void ESP32BLEController::ESP32BLEController::set_wifi_configuration(const string& ssid, const string& password, bool hidden_network) {
  wifi_configuration_handler.set_credentials(ssid, password, hidden_network);
}

void ESP32BLEController::ESP32BLEController::clear_wifi_configuration_and_reboot() {
  wifi_configuration_handler.clear_credentials();

  App.safe_reboot();
}

const optional<string> ESP32BLEController::ESP32BLEController::get_current_ssid_in_wifi_configuration() {
  return wifi_configuration_handler.get_current_ssid();
}
#endif

void ESP32BLEController::send_command_result(const string& result_message) {
  maintenance_handler->send_command_result(result_message);
}

void ESP32BLEController::send_command_result(const char* format, ...) {
  char buffer[128];
  va_list arg;
  va_start(arg, format);
  vsnprintf(buffer, sizeof(buffer), format, arg);
  va_end(arg);
  
  maintenance_handler->send_command_result(buffer);
}

#ifdef USE_BINARY_SENSOR
  void ESP32BLEController::on_binary_sensor_update(binary_sensor::BinarySensor *obj, bool state) { update_component_state(obj, state); }
#endif
#ifdef USE_COVER
  void ESP32BLEController::on_cover_update(cover::Cover *obj) {}
#endif
#ifdef USE_FAN
  void ESP32BLEController::on_fan_update(fan::Fan *fan) { update_component_state(fan, fan->state); }
#endif
#ifdef USE_LIGHT
  void ESP32BLEController::on_light_update(light::LightState *obj) {}
#endif
#ifdef USE_SENSOR
  void ESP32BLEController::on_sensor_update(sensor::Sensor *component, float state) { update_component_state(component, state); }
#endif
#ifdef USE_SWITCH
  void ESP32BLEController::on_switch_update(switch_::Switch *obj, bool state) { update_component_state(obj, state); }
#endif
#ifdef USE_TEXT_SENSOR
  void ESP32BLEController::on_text_sensor_update(text_sensor::TextSensor *obj, std::string state) { update_component_state(obj, state); }
#endif
#ifdef USE_CLIMATE
  void ESP32BLEController::on_climate_update(climate::Climate *obj) {}
#endif

template <typename C, typename S> 
void ESP32BLEController::update_component_state(C* component, S state) {
  static_assert(std::is_base_of<EntityBase, C>::value, "EntityBase subclasses expected");

  auto object_id = component->get_object_id();
  BLEComponentHandlerBase* handler = handler_for_component[object_id];
  if (handler != nullptr) {
    handler->send_value(state);
  }
}

void ESP32BLEController::execute_in_loop(std::function<void()>&& deferred_function) {
  bool ok = deferred_functions_for_loop.push(std::move(deferred_function));
  if (!ok) {
    ESP_LOGW(TAG, "Deferred functions queue full");
  }
}

void ESP32BLEController::loop() {
  std::function<void()> deferred_function;
  while (deferred_functions_for_loop.take(deferred_function)) {
    deferred_function();
  }
}

void ESP32BLEController::configure_ble_security() {
  if (get_security_mode() == BLESecurityMode::NONE) {
    return;
  }

  ESP_LOGD(TAG, "  Setting up BLE security");

  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
  BLEDevice::setSecurityCallbacks(this);

  // see https://github.com/espressif/esp-idf/blob/b0150615dff529662772a60dcb57d5b559f480e2/examples/bluetooth/bluedroid/ble/gatt_security_server/tutorial/Gatt_Security_Server_Example_Walkthrough.md
  BLESecurity security;
  security.setAuthenticationMode(get_security_mode() == BLESecurityMode::BOND ? ESP_LE_AUTH_BOND : ESP_LE_AUTH_REQ_SC_MITM_BOND);
  security.setCapability(can_show_pass_key ? ESP_IO_CAP_OUT : ESP_IO_CAP_NONE);
  security.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security.setKeySize(16);

  uint8_t auth_option = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_ENABLE;
  esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH, &auth_option, sizeof(uint8_t));
}

void ESP32BLEController::onPassKeyNotify(uint32_t pass_key) {
  char pass_key_digits[6 + 1];
  snprintf(pass_key_digits, sizeof(pass_key_digits), "%06d", pass_key);
  string pass_key_str(pass_key_digits);

  auto& callbacks = on_show_pass_key_callbacks;
  global_ble_controller->execute_in_loop([&callbacks, pass_key_str](){ 
    ESP_LOGI(TAG, "BLE authentication - pass received");
    callbacks.call(pass_key_str);
  });
}

void ESP32BLEController::onAuthenticationComplete(esp_ble_auth_cmpl_t result) {
  auto& callbacks = on_authentication_complete_callbacks;
  bool success=result.success;
  global_ble_controller->execute_in_loop([&callbacks, success](){
    if (success) {
      ESP_LOGD(TAG, "BLE authentication - completed succesfully");
    } else {
      ESP_LOGD(TAG, "BLE authentication - failed");
    }
    callbacks.call(success);
  });
}

uint32_t ESP32BLEController::onPassKeyRequest() {
  global_ble_controller->execute_in_loop([](){ ESP_LOGD(TAG, "onPassKeyRequest"); });
  return 123456;
}

bool ESP32BLEController::onSecurityRequest() {
  global_ble_controller->execute_in_loop([](){ ESP_LOGD(TAG, "onSecurityRequest"); });
  return true;
}

bool ESP32BLEController::onConfirmPIN(uint32_t pin) {
  global_ble_controller->execute_in_loop([](){ ESP_LOGD(TAG, "onConfirmPIN"); });
  return true;
}

void ESP32BLEController::ESP32BLEController::onConnect(BLEServer* server) {
  auto& callbacks = on_connected_callbacks;
  global_ble_controller->execute_in_loop([&callbacks](){ 
    ESP_LOGD(TAG, "BLE server - connected");
    callbacks.call();
  });
}

void ESP32BLEController::ESP32BLEController::onDisconnect(BLEServer* server) {
  auto& callbacks = on_disconnected_callbacks;
  global_ble_controller->execute_in_loop([&callbacks, this](){ 
    ESP_LOGD(TAG, "BLE server - disconnected");

    // after 500ms start advertising again
    const uint32_t delay_millis = 500;
    App.scheduler.set_timeout(this, "", delay_millis, []{ BLEDevice::startAdvertising(); });

    callbacks.call(); 
  });
}

ESP32BLEController* global_ble_controller = nullptr;

} // namespace esp32_ble_controller
} // namespace esphome
