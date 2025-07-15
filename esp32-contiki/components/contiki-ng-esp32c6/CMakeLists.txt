# components/contiki-ng-esp32c6/CMakeLists.txt
set(CONTIKI_BASE ${CMAKE_CURRENT_LIST_DIR}/contiki-ng)
set(PORT_ARCH    ${CMAKE_CURRENT_LIST_DIR}/arch)
set(PORT_PLAT    ${CMAKE_CURRENT_LIST_DIR}/platform)
set(PORT_PROJ    ${CMAKE_CURRENT_LIST_DIR}/project)
set(CONTIKI_APP  ${CONTIKI_BASE}/examples/rpl-udp/udp-client.c)

# ---- 1. pick *generic* Contiki sources (skip other platforms) ----
file(GLOB_RECURSE CONTIKI_CORE
     ${CONTIKI_BASE}/os/*.c
     ${CONTIKI_BASE}/os/lib/*.c
     ${CONTIKI_BASE}/os/services/*.c
     ${CONTIKI_BASE}/os/net/*.c
     ${CONTIKI_APP}
)
list(FILTER CONTIKI_CORE EXCLUDE REGEX "/arch/")   # drop foreign arch
list(FILTER CONTIKI_CORE EXCLUDE REGEX "/contiki-main\\.c$")
list(FILTER CONTIKI_CORE EXCLUDE REGEX "/os/lib/fs/fat/")
# components/contiki-ng-esp32c6/CMakeLists.txt
list(FILTER CONTIKI_CORE EXCLUDE REGEX "/os/dev/spi\\.c$")
list(FILTER CONTIKI_CORE EXCLUDE REGEX "/os/dev/spi-arch/")   # if any
list(FILTER CONTIKI_CORE EXCLUDE REGEX "/os/lib/fs/fat/")
list(FILTER CONTIKI_CORE EXCLUDE REGEX "/os/net/mac/tsch/.*\\.c$")
list(FILTER CONTIKI_CORE EXCLUDE REGEX "/os/net/ipv6/multicast/.*\\.c$")
list(FILTER CONTIKI_CORE EXCLUDE REGEX "/os/net/app-layer/coap/mbedtls-support/")
list(FILTER CONTIKI_CORE EXCLUDE REGEX "/os/storage/")
list(FILTER CONTIKI_CORE EXCLUDE REGEX "/os/services/at-master/")
list(FILTER CONTIKI_CORE EXCLUDE REGEX "/os/services/orchestra/")
list(FILTER CONTIKI_CORE EXCLUDE REGEX "/os/services/tsch-cs/")
list(FILTER CONTIKI_CORE EXCLUDE REGEX "/os/services/ip64/")
list(FILTER CONTIKI_CORE EXCLUDE REGEX "/os/services/ecc/")
list(FILTER CONTIKI_CORE EXCLUDE REGEX "/os/services/rpl-border-router/")
list(FILTER CONTIKI_CORE EXCLUDE REGEX "/os/services/simple-energest/")
# keep Lite, drop Classic
list(FILTER CONTIKI_CORE EXCLUDE REGEX "/os/net/routing/rpl-classic/")

# ---- 2. add your ESP32-C6 specific sources ----
file(GLOB PORT_SRC
     ${PORT_ARCH}/*.c
     ${PORT_PLAT}/*.c
     ${PORT_PROJ}/*.c
)

# CMakeLists.txt ― create ${CMAKE_BINARY_DIR}/random_wrap/random.h
set(WRAP_DIR "${CMAKE_CURRENT_BINARY_DIR}/lib_wrap")
file(MAKE_DIRECTORY "${WRAP_DIR}")
# Collect the headers inside os/lib
file(GLOB LIB_HEADERS "${CONTIKI_BASE}/os/lib/*.h")
foreach(hdr IN LISTS LIB_HEADERS)
  if(NOT hdr MATCHES "/assert\\.h$")                #skip assert.h to avoid conflict
    # Grab just the filename part (random.h, memb.h, …)
    get_filename_component(fname "${hdr}" NAME)     # ← CMake-native, not $(basename)
    # Copy each header into lib_wrap/
    configure_file("${hdr}" "${WRAP_DIR}/${fname}" COPYONLY)
  endif()
endforeach()

#set(RANDOM_WRAP ${CMAKE_CURRENT_BINARY_DIR}/random_wrap)
#file(MAKE_DIRECTORY ${RANDOM_WRAP})
#configure_file(${CONTIKI_BASE}/os/lib/random.h
#               ${RANDOM_WRAP}/random.h COPYONLY)

idf_component_register(
    SRCS          ${CONTIKI_CORE} ${PORT_SRC} ${PORT_PROJ}
    INCLUDE_DIRS  ${CONTIKI_BASE}
                  ${CONTIKI_BASE}/os
                  ${CONTIKI_BASE}/os/dev
                  ${PORT_PROJ}               
                  ${PORT_ARCH}
                  ${PORT_PLAT}       
                  ${WRAP_DIR}           
                  ${CONTIKI_BASE}/os/sys
                  ${CONTIKI_BASE}/os/net
                  ${CONTIKI_BASE}/os/net/routing
                  ${CONTIKI_BASE}/os/net/routing/rpl-lite
                  ${CONTIKI_BASE}/os/net/routing/rpl-classic
                  ${CONTIKI_BASE}/os/net/ipv6
                  ${CONTIKI_BASE}/os/lib/dbg-io
                  ${CONTIKI_BASE}/os/services
                  ${CONTIKI_BASE}/os/services/resolv
                  ${CONTIKI_BASE}/os/services/lwm2m
                  ${CONTIKI_BASE}/os/services/slip-cmd
#                  ${CONTIKI_BASE}/os/services/ip64
                  ${CONTIKI_BASE}/os/net/app-layer/coap
    REQUIRES      freertos esp_timer mbedtls ieee802154 # or whatever you need
)

# suppress the protothread fall-through warning only for this library
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-implicit-fallthrough)

# Tell the core which project-conf to use
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PROJECT_CONF_H=\"project-conf.h\")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=format-truncation)

target_compile_definitions(${COMPONENT_LIB} PRIVATE NDEBUG)
