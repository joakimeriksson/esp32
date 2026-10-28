cmake_minimum_required(VERSION 3.16)
# the worker, USB-MIDI host, WiFi, catalog and card mirror are fm1-flasher-p4's components
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../fm1-flasher-p4/components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_dock)
