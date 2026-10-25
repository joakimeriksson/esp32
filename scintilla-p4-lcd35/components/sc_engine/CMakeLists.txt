# Scintilla's engine (../../engine, a git subtree of scintilla-fm1's engine/) as an IDF component,
# unchanged. -O2 for the render loop; it is integer-only, so no floating-point flags matter.
idf_component_register(SRCS "../../engine/sc_voice.c" "../../engine/sc_synth.c"
                       INCLUDE_DIRS "../../engine")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-function)
