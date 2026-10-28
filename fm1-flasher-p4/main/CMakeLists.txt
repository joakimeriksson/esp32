idf_component_register(SRCS "main.c" "ui.c" "board.c" INCLUDE_DIRS "." REQUIRES flasher WHOLE_ARCHIVE)   # the board and the screen are called from the flasher component: every object in
