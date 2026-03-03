import esphome.codegen as cg
from esphome.components.esp32 import (
    VARIANT_ESP32,
    VARIANT_ESP32C2,
    VARIANT_ESP32C3,
    VARIANT_ESP32C5,
    VARIANT_ESP32C6,
    VARIANT_ESP32H2,
    VARIANT_ESP32S3,
    add_idf_component,
    add_idf_sdkconfig_option,
    only_on_variant,
)
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE, coroutine_with_priority
from esphome.helpers import write_file_if_changed

CODEOWNERS = ["@nikhiljha"]

# Matter replaces the ESPHome native API - they serve the same purpose
CONFLICTS_WITH = ["api"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["network"]

CONF_DISCRIMINATOR = "discriminator"
CONF_PASSCODE = "passcode"
CONF_VENDOR_ID = "vendor_id"
CONF_PRODUCT_ID = "product_id"
CONF_PRODUCT_NAME = "product_name"

matter_ns = cg.esphome_ns.namespace("matter")
MatterComponent = matter_ns.class_("MatterComponent", cg.Component, cg.Controller)


def _validate_passcode(value):
    """Validate Matter setup passcode per spec constraints."""
    value = cv.uint32_t(value)
    # Matter spec invalid passcodes
    invalid = {
        0,
        11111111,
        22222222,
        33333333,
        44444444,
        55555555,
        66666666,
        77777777,
        88888888,
        99999999,
        12345678,
        87654321,
    }
    if value in invalid:
        raise cv.Invalid(f"Passcode {value} is not allowed by the Matter specification")
    if value > 99999998:
        raise cv.Invalid("Passcode must be between 1 and 99999998")
    return value


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(MatterComponent),
            cv.Optional(CONF_DISCRIMINATOR, default=3840): cv.int_range(
                min=0, max=4095
            ),
            cv.Optional(CONF_PASSCODE, default=20202021): _validate_passcode,
            cv.Optional(CONF_VENDOR_ID, default=0xFFF1): cv.hex_uint16_t,
            cv.Optional(CONF_PRODUCT_ID, default=0x8000): cv.hex_uint16_t,
            cv.Optional(CONF_PRODUCT_NAME): cv.string_strict,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    only_on_variant(
        supported=[
            VARIANT_ESP32,
            VARIANT_ESP32C2,
            VARIANT_ESP32C3,
            VARIANT_ESP32C5,
            VARIANT_ESP32C6,
            VARIANT_ESP32H2,
            VARIANT_ESP32S3,
        ]
    ),
)


def _set_matter_sdkconfig(config):
    """Set ESP-IDF sdkconfig options required by the Matter stack."""
    # Core Matter/CHIP config
    add_idf_sdkconfig_option("CONFIG_CHIP_TASK_STACK_SIZE", 8192)
    add_idf_sdkconfig_option("CONFIG_CHIP_ENABLE_PAIRING_AUTOSTART", True)

    # BLE for commissioning
    add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_ENABLED", True)

    # Increase NVS partition for Matter fabric storage
    add_idf_sdkconfig_option("CONFIG_NVS_ENCRYPTION", False)

    # mbedTLS settings required by Matter
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_HKDF_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_ECJPAKE_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_HARDWARE_AES", True)

    # Increase main task stack for Matter init
    add_idf_sdkconfig_option("CONFIG_ESP_MAIN_TASK_STACK_SIZE", 8192)

    # LWIP settings for Matter
    add_idf_sdkconfig_option("CONFIG_LWIP_IPV6", True)
    add_idf_sdkconfig_option("CONFIG_LWIP_MULTICAST_PING", True)

    # Matter OTA requestor
    add_idf_sdkconfig_option("CONFIG_ENABLE_OTA_REQUESTOR", True)


@coroutine_with_priority(40.0)
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Track controller registration for entity state updates
    CORE.register_controller()

    cg.add_define("USE_MATTER")

    # connectedhomeip headers require CHIP_HAVE_CONFIG_H to properly include
    # platform build config (SystemBuildConfig.h, CHIPDeviceBuildConfig.h).
    # Without this, CHIP_SYSTEM_CONFIG_USE_SOCKETS etc. aren't set, causing
    # override errors in SystemLayerImplSelect.h and other headers.
    # The connectedhomeip CMake normally sets this for dependent components,
    # but PlatformIO doesn't propagate it to the src component.
    cg.add_build_flag("-DCHIP_HAVE_CONFIG_H=1")

    # Set Matter configuration
    cg.add(var.set_discriminator(config[CONF_DISCRIMINATOR]))
    cg.add(var.set_passcode(config[CONF_PASSCODE]))
    cg.add(var.set_vendor_id(config[CONF_VENDOR_ID]))
    cg.add(var.set_product_id(config[CONF_PRODUCT_ID]))

    if CONF_PRODUCT_NAME in config:
        cg.add(var.set_product_name(config[CONF_PRODUCT_NAME]))

    # Add esp_matter as an IDF component dependency
    add_idf_component(
        name="espressif/esp_matter",
        ref="1.4.0",
    )

    # Write the top-level CMakeLists.txt with EXECUTABLE_COMPONENT_NAME=src
    # BEFORE PlatformIO gets a chance to generate its default one.
    # PlatformIO's create_default_project_files() only writes CMakeLists.txt
    # if it doesn't already exist, so by writing it here during ESPHome's
    # code generation phase, we ensure our version is used.
    # This is needed because esp_matter's CMakeLists.txt expects
    # EXECUTABLE_COMPONENT_NAME to be set (defaults to "main" which doesn't
    # exist in ESPHome's build structure - ESPHome uses "src").
    cmake_path = CORE.relative_build_path("CMakeLists.txt")
    cmake_content = (
        f'cmake_minimum_required(VERSION 3.16.0)\n'
        f'set(EXECUTABLE_COMPONENT_NAME "src")\n'
        f'include($ENV{{IDF_PATH}}/tools/cmake/project.cmake)\n'
        f'project({CORE.name})\n'
    )
    write_file_if_changed(cmake_path, cmake_content)

    # Write a custom src/CMakeLists.txt that declares dependency on esp_matter.
    # PlatformIO's default src/CMakeLists.txt uses a bare idf_component_register()
    # without REQUIRES, which means the src component doesn't get the compile
    # definitions from esp_matter/connectedhomeip (like CHIP_HAVE_CONFIG_H).
    # This causes connectedhomeip headers to compile with wrong preprocessor
    # state, leading to override errors in SystemLayerImplSelect.h etc.
    # PlatformIO only writes src/CMakeLists.txt if it doesn't already exist,
    # so pre-writing it here ensures our version with REQUIRES is used.
    src_cmake_path = CORE.relative_src_path("CMakeLists.txt")
    src_cmake_content = (
        f'FILE(GLOB_RECURSE app_sources ${{CMAKE_SOURCE_DIR}}/src/*.*)\n'
        f'idf_component_register(\n'
        f'    SRCS ${{app_sources}}\n'
        f'    PRIV_REQUIRES espressif__esp_matter\n'
        f')\n'
    )
    write_file_if_changed(src_cmake_path, src_cmake_content)

    # Set required sdkconfig options
    _set_matter_sdkconfig(config)
