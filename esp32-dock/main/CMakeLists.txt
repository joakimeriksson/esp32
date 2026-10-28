set(embed "")
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/../probes/browser.dock)
    set(embed "../probes/browser.dock")   # the browser probe (fm1-dock's probes/browser.c), built in
endif()
idf_component_register(SRCS "main.c" "board.c" "ui_log.c" "console.c" INCLUDE_DIRS "." EMBED_FILES ${embed} WHOLE_ARCHIVE)   # ui_log.c and board.c are called from the flasher component: every object in
if(embed)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DOCK_BROWSER_EMBEDDED=1)
endif()
# the board: the ESP32-P4-WIFI6 (default), or -DDOCK_BOARD=lcd35 for the Touch-LCD-3.5 with its screen off
if(DOCK_BOARD STREQUAL "lcd35")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DOCK_BOARD_LCD35=1)
endif()
