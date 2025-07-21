idf_component_register(SRCS "app_main.c"
                              "LCD_Driver/Vernon_ST7789T/Vernon_ST7789T.c" 
                              "LCD_Driver/ST7789.c"
                              "LVGL_Driver/LVGL_Driver.c"
                              "ieee_scan.c"
                              "ui_spectrum.c"
                              "RGB/RGB.c"
                    INCLUDE_DIRS
			                  "./LCD_Driver/Vernon_ST7789T" 
                              "./LCD_Driver" 
                              "./LVGL_Driver"
                              "./RGB" 
                              "."
	            )
