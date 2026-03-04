import os

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
    include_builtin_idf_component,
    only_on_variant,
    require_vfs_select,
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


def _patch_openthread_build_datetime():
    """Patch ESP-IDF's OpenThread CMakeLists.txt to fix PlatformIO quoting bug.

    PlatformIO fails to properly quote the OPENTHREAD_BUILD_DATETIME macro
    when it contains spaces (e.g., " 2026-03-04 06:52:00 UTC"), causing a
    compile error in instance_api.cpp. We patch the timestamp format to use
    ISO 8601 compact format without spaces.
    """
    idf_path = os.environ.get("IDF_PATH")
    if idf_path is None:
        # Try PlatformIO's framework path
        pio_packages = os.path.expanduser("~/.platformio/packages")
        idf_path = os.path.join(pio_packages, "framework-espidf")

    ot_cmake = os.path.join(idf_path, "components", "openthread", "CMakeLists.txt")
    if not os.path.exists(ot_cmake):
        return

    with open(ot_cmake, "r") as f:
        content = f.read()

    old = 'string(TIMESTAMP OT_BUILD_TIMESTAMP " %Y-%m-%d %H:%M:%S UTC" UTC)'
    new = 'string(TIMESTAMP OT_BUILD_TIMESTAMP "%Y%m%dT%H%M%SZ" UTC)'

    if old in content:
        content = content.replace(old, new)
        with open(ot_cmake, "w") as f:
            f.write(content)


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

    # Verbose CHIP logging to debug commissioning issues
    # Keep global ESP-IDF log at INFO to avoid overwhelming early init,
    # but set CHIP/Matter-specific logging to Detail for commissioning diagnostics
    add_idf_sdkconfig_option("CONFIG_LOG_DEFAULT_LEVEL", 3)  # ESP_LOG_INFO (default)
    add_idf_sdkconfig_option("CONFIG_LOG_MAXIMUM_LEVEL", 4)  # Allow up to DEBUG
    add_idf_sdkconfig_option("CONFIG_CHIP_LOG_FILTERING", False)
    add_idf_sdkconfig_option("CONFIG_MATTER_LOG_LEVEL", 4)  # Detail

    # BLE for commissioning - BLE-only mode frees memory from Classic BT
    add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_CTRL_MODE_BLE_ONLY", True)
    add_idf_sdkconfig_option("CONFIG_BT_CTRL_HCI_MODE_VHCI", True)
    # Larger NimBLE task stack for SPAKE2+ crypto during PASE handshake
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_TASK_STACK_SIZE", 6144)
    # Allow enough ACL connections for commissioning
    add_idf_sdkconfig_option("CONFIG_BT_ACL_CONNECTIONS", 3)

    # Increase NVS partition for Matter fabric storage
    add_idf_sdkconfig_option("CONFIG_NVS_ENCRYPTION", False)

    # mbedTLS settings required by Matter - hardware acceleration is critical
    # to complete the SPAKE2+/PASE handshake within the controller's timeout
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_HKDF_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_ECJPAKE_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_HARDWARE_AES", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_HARDWARE_MPI", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_HARDWARE_SHA", True)

    # Increase main task stack for Matter init
    add_idf_sdkconfig_option("CONFIG_ESP_MAIN_TASK_STACK_SIZE", 8192)

    # LWIP settings for Matter - larger buffers for IPv6 multicast during discovery
    add_idf_sdkconfig_option("CONFIG_LWIP_IPV6", True)
    add_idf_sdkconfig_option("CONFIG_LWIP_MULTICAST_PING", True)
    add_idf_sdkconfig_option("CONFIG_LWIP_TCPIP_RECVMBOX_SIZE", 32)
    add_idf_sdkconfig_option("CONFIG_LWIP_UDP_RECVMBOX_SIZE", 32)
    add_idf_sdkconfig_option("CONFIG_LWIP_MAX_SOCKETS", 16)

    # Matter OTA requestor
    add_idf_sdkconfig_option("CONFIG_ENABLE_OTA_REQUESTOR", True)

    # If WiFi is not loaded, enable OpenThread for Matter over Thread.
    # The Matter/CHIP stack handles Thread initialization and credential
    # provisioning during commissioning — no pre-configured Thread dataset needed.
    if "wifi" not in CORE.loaded_integrations:
        # OpenThread radio and stack
        add_idf_sdkconfig_option("CONFIG_IEEE802154_ENABLED", True)
        add_idf_sdkconfig_option("CONFIG_OPENTHREAD_ENABLED", True)
        add_idf_sdkconfig_option("CONFIG_OPENTHREAD_FTD", True)
        add_idf_sdkconfig_option("CONFIG_OPENTHREAD_RADIO_NATIVE", True)
        add_idf_sdkconfig_option("CONFIG_OPENTHREAD_CLI", False)
        add_idf_sdkconfig_option("CONFIG_OPENTHREAD_CONSOLE_ENABLE", False)
        add_idf_sdkconfig_option("CONFIG_OPENTHREAD_DIAG", False)
        add_idf_sdkconfig_option("CONFIG_OPENTHREAD_DNS64_CLIENT", True)
        add_idf_sdkconfig_option("CONFIG_OPENTHREAD_SRP_CLIENT", True)
        add_idf_sdkconfig_option("CONFIG_OPENTHREAD_SRP_CLIENT_MAX_SERVICES", 5)
        # Disable WiFi in CHIP for Thread-only mode.
        # CHIP's ConnectivityManagerImpl requires WiFi and Thread endpoint IDs
        # to be different. Setting WiFi endpoint to 0xFFFE avoids the conflict.
        add_idf_sdkconfig_option("CONFIG_ENABLE_WIFI_STATION", False)
        add_idf_sdkconfig_option("CONFIG_ENABLE_WIFI_AP", False)
        # Thread network commissioning endpoint must match the root node (endpoint 0)
        # where esp_matter places the NetworkCommissioning cluster.
        # CHIP's GenericThreadDriver registers on this endpoint ID, so if it
        # doesn't match the cluster's endpoint, attribute reads will fail.
        add_idf_sdkconfig_option("CONFIG_THREAD_NETWORK_ENDPOINT_ID", 0)
        add_idf_sdkconfig_option("CONFIG_WIFI_NETWORK_ENDPOINT_ID", 0xFFFE)
        # Tell esp_matter to initialize Thread stack during esp_matter::start()
        add_idf_sdkconfig_option("CONFIG_ESP_MATTER_ENABLE_OPENTHREAD", True)
        # Critical: CHIP_DEVICE_CONFIG_ENABLE_THREAD is derived from this.
        # Without it, CHIP thinks Thread is disabled and NetworkCommissioning
        # cluster defaults to WiFi mode (which doesn't exist).
        add_idf_sdkconfig_option("CONFIG_ENABLE_MATTER_OVER_THREAD", True)
        # Enable Thread network commissioning driver so CHIP can provision
        # Thread credentials during Matter commissioning.
        add_idf_sdkconfig_option("CONFIG_THREAD_NETWORK_COMMISSIONING_DRIVER", True)
        # Disable minimal mDNS (UDP multicast) for Thread mode.
        # Minimal mDNS requires network interfaces to broadcast on, but Thread
        # devices have no IP interface until they join the Thread network.
        # With this disabled, CHIP uses the platform DNS-SD implementation
        # (DnssdImpl.cpp) which dispatches to OpenThread's SRP client for
        # service registration through the Thread Border Router.
        add_idf_sdkconfig_option("CONFIG_USE_MINIMAL_MDNS", False)


@coroutine_with_priority(40.0)
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Track controller registration for entity state updates
    CORE.register_controller()

    cg.add_define("USE_MATTER")

    # For Thread mode (no WiFi), esp_matter's connectedhomeip pulls in
    # OpenThread when CONFIG_OPENTHREAD_ENABLED=y. VFS select is needed
    # for OpenThread's eventfd-based event loop.
    if "wifi" not in CORE.loaded_integrations:
        cg.add_define("USE_MATTER_THREAD")
        require_vfs_select()
        _patch_openthread_build_datetime()

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
    # OT_BUILD_TIMESTAMP must be set before OpenThread's CMake runs.
    # OpenThread only sets it if not already defined, and PlatformIO
    # fails to properly quote the default (date string with spaces).
    # Using a simple string avoids the quoting issue entirely.
    thread_cmake = ""
    if "wifi" not in CORE.loaded_integrations:
        thread_cmake = 'set(OT_BUILD_TIMESTAMP "esphome")\n'
    cmake_content = (
        f'cmake_minimum_required(VERSION 3.16.0)\n'
        f'{thread_cmake}'
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
