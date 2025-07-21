#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define CH_FIRST     11
#define CH_LAST      26
#define ED_WIN_SYM   40          // 40 × 16 µs ≈ 0.64 ms

typedef struct { uint8_t ch; int8_t pwr; } ed_point_t;
extern QueueHandle_t ieee_scan_get_queue(void);
