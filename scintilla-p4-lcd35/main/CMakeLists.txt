idf_component_register(SRCS "main.c" "audio_task.c" "ui.c" "events.c" "ble_midi.c" INCLUDE_DIRS ".")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)
