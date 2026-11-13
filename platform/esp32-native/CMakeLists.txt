# Daytona USA for the EspRacer console (ESP32-S3-Touch-LCD-1.83).
#   . ~/esp/v6.0/esp-idf/export.sh; idf.py -B build-racer build     (or EspRacer/tools/build_all.sh daytona)
# EspRacer is found by $ESPRACER_DIR, else as a sibling of this repository.
cmake_minimum_required(VERSION 3.22)
if(DEFINED ENV{ESPRACER_DIR})
  set(ESPRACER_DIR $ENV{ESPRACER_DIR})
else()
  get_filename_component(ESPRACER_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../EspRacer ABSOLUTE)
endif()
set(SDKCONFIG_DEFAULTS "${ESPRACER_DIR}/sdkconfig.common;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
set(EXTRA_COMPONENT_DIRS ${ESPRACER_DIR}/components)
set(SDKCONFIG ${CMAKE_BINARY_DIR}/sdkconfig)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(daytona)
