/*
 * Very small debug‐print back-end for Contiki-NG on ESP32-C6
 * ----------------------------------------------------------
 *  - dbg_putchar()      : send one char via the ROM UART helper
 *  - dbg_send_bytes()   : send a raw buffer       ”       ”
 *
 *  They are only used by os/lib/dbg-io/puts.c and a few legacy apps.
 *  If you later want timestamps, colours, etc. just replace the two
 *  bodies below – the signatures must stay the same.
 */
#include "contiki.h"
#include "dbg.h"
#include "hal/uart_ll.h"
#include "esp_rom_uart.h"

#ifndef DBG_UART_NUM
/* Use the console UART that IDF already initialised (usually 0) */
#define DBG_UART_NUM   UART_NUM_0
#endif

/*---------------------------------------------------------------------------*/
int
dbg_putchar(int c)
{
  /* Blocking transmit; fine for tiny debug messages                       */
  esp_rom_output_tx_wait_idle(DBG_UART_NUM);
  esp_rom_output_tx_one_char(c);
  return c;
}
/*---------------------------------------------------------------------------*/
unsigned int
dbg_send_bytes(const unsigned char *s, unsigned int len)
{
  unsigned int i = 0;

  while(s && *s != 0) {
    if(i >= len) {
      break;
    }
    dbg_putchar(*s++);
    i++;
  }
  return i;
}
/*---------------------------------------------------------------------------*/