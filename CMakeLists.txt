# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ESP-IDF Components library -- needed for DHT driver
set(EXTRA_COMPONENT_DIRS ../esp-idf-lib/components)


# Get network settings from environment variables 
set(WLAN_SSID $ENV{ESP_WSTATION__WLAN_SSID})
if (NOT DEFINED WLAN_SSID)
	message(FATAL_ERROR "'ESP_WSTATION__WLAN_SSID' is unset")
endif()

set(WLAN_KEY $ENV{ESP_WSTATION__WLAN_KEY})
if (NOT DEFINED WLAN_KEY)
	message(FATAL_ERROR "'ESP_WSTATION__WLAN_KEY' is unset")
endif()

set(HOSTNAME $ENV{ESP_WSTATION__HOSTNAME})
if (NOT DEFINED HOSTNAME)
	message(FATAL_ERROR "'ESP_WSTATION__HOSTNAME' is unset")
endif()

# Yes this is ugly.... Not sure if there is a better way, but
# I was unable to get it to work using other cmake commands.
idf_build_get_property(comp_defs COMPILE_DEFINITIONS)
idf_build_set_property(COMPILE_DEFINITIONS "${comp_defs};-DWLAN_SSID=\"${WLAN_SSID}\";-DWLAN_KEY=\"${WLAN_KEY}\";-DHOSTNAME=\"${HOSTNAME}\"")

project(esp-wstation)