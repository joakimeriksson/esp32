/* radio-esp32c6.c  – minimal 802.15.4 driver for Contiki-NG */
#include "contiki.h"
#include "dev/radio.h"
#include "net/packetbuf.h"
#include "net/netstack.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ieee802154.h"         /* APIs */
#include "esp_ieee802154_types.h"   /* data structures & cb typedefs */
#include "esp_timer.h" 

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#include "esp_log.h"


static const char *TAG = "ESP RADIO";

#define RX_BUF_LEN     128     /* MAC max 127 + CRC */

/* Log configuration */
#include "sys/log.h"
#define LOG_MODULE "esp32-rf"
#define LOG_LEVEL LOG_LEVEL_NONE


/*---------------------------------------------------------------------------*/
PROCESS(esp_ieee802154_process, "ESP32-C6 radio");
/*---------------------------------------------------------------------------*/

/* RX bookkeeping */
static uint8_t  rx_buf[RX_BUF_LEN];
static uint16_t rx_len;
static volatile bool rx_pending;
static int8_t  rx_rssi;
static uint8_t rx_lqi;
static uint8_t tx_status = RADIO_TX_ERR;
static uint32_t rx_timestamp;  /* in us, from the radio's internal timer */

enum radio_state_e {
  RADIO_STATE_RECEIVING,
  RADIO_STATE_TRANSMITTING,
  RADIO_STATE_IDLE,
  RADIO_STATE_OFF
} radio_state = RADIO_STATE_IDLE;

static char *
get_state_string(void)
{
  switch(radio_state) {
    case RADIO_STATE_RECEIVING: return "RECEIVING";
    case RADIO_STATE_TRANSMITTING: return "TRANSMITTING";
    case RADIO_STATE_IDLE: return "IDLE";
    case RADIO_STATE_OFF: return "OFF";
    default: return "UNKNOWN";
  }
} /* state_string() */

static uint64_t sfd_time_us = 0; /* Timestamp when SFD was received */
static enum radio_state_e get_radio_state(void)
{
  if(radio_state == RADIO_STATE_RECEIVING) {
    if(esp_timer_get_time() - sfd_time_us > 1000) {
      /* If we have not received SFD in a while, assume we are idle */
      radio_state = RADIO_STATE_IDLE;
      ESP_EARLY_LOGI(TAG, "Radio state changed to IDLE due to timeout");
    }
  }
  return radio_state;
} /* get_radio_state() */

static bool
add_packet_to_buf(const uint8_t *data, size_t len, int8_t rssi, uint8_t lqi, uint32_t timestamp)
{
  if(len > RX_BUF_LEN) {
    ESP_LOGE(TAG, "Packet too long: %u bytes", (unsigned int)len);
    return false;
  }
  memcpy(rx_buf, data, len);
  rx_len = len;
  rx_rssi = rssi;
  rx_lqi = lqi;
  rx_timestamp = timestamp;

  rx_pending = true; /* Indicate that a packet is ready */
  return true;
} /* add_packet_to_buf() */

static int
get_packet_from_buf(uint8_t *buf, unsigned int len)
{
  if(!rx_pending) {
    ESP_LOGE(TAG, "No packet available");
    return 0;
  }
  if(len < rx_len) {
    ESP_LOGE(TAG, "Buffer too small: %u < %u", (unsigned int)len, (unsigned int)rx_len);
    return 0;
  }
  memcpy(buf, rx_buf, rx_len);

  rx_pending = false; /* Clear the pending flag */
  return len;
} /* get_packet_from_buf() */

/*---------------------------------------------------------------------------*/
/* ISR-level callback from the Espressif driver                              */
static IRAM_ATTR void
rx_done_cb(uint8_t *frame, esp_ieee802154_frame_info_t *info)
{
  /* info->len includes FCS */
  size_t len = frame[0];
  radio_state = RADIO_STATE_IDLE;
 
  if(len > RX_BUF_LEN) return;               /* drop oversize */
  /* remove lenght byte */
  add_packet_to_buf(frame + 1, len - 2, info->rssi, info->lqi, info->timestamp); /* -2 for length byte and FCS */

  ESP_EARLY_LOGI(TAG, "RX: %u bytes, RSSI %d dBm, LQI %u, timestamp %u us (channel %u)",
                 rx_len, rx_rssi, rx_lqi, rx_timestamp, info->channel);
  process_poll(&esp_ieee802154_process);     /* wake the driver process */
  esp_ieee802154_receive_handle_done(frame);
}
/*---------------------------------------------------------------------------*/
static IRAM_ATTR void
rx_sfd_done_cb(void)
{
  /* This callback is called when the SFD of the frame is received */
  ESP_EARLY_LOGI(TAG, "RX SFD received");
  /* You can handle SFD events here if needed */
  sfd_time_us = esp_timer_get_time(); /* Store the timestamp of SFD reception */
  radio_state = RADIO_STATE_RECEIVING; /* Update state if needed */
}

/*---------------------------------------------------------------------------*/
static IRAM_ATTR void ed_done_cb(int8_t power_dbm)
{
    /* called from the interrupt handler – keep it short */
    ESP_EARLY_LOGI(TAG, "ED: power = %d dBm (%d)", power_dbm, esp_ieee802154_get_channel());
}
/*---------------------------------------------------------------------------*/
static IRAM_ATTR void tx_done_cb(const uint8_t *psdu,
                                 const uint8_t *ack,
                                 esp_ieee802154_frame_info_t *ack_info)
{
    ESP_EARLY_LOGI("TX", "Packet sent, ack=%s",
                   ack ? "yes" : "no");
    radio_state = RADIO_STATE_IDLE;
    if(ack) {
        ESP_EARLY_LOGI(TAG, "Ack received: RSSI %d dBm, LQI %u Ack Byte %02x%02x%02x%02x%02x",
                       ack_info->rssi, ack_info->lqi, ack[0], ack[1], ack[2], ack[3], ack[4]);
      tx_status = RADIO_TX_OK;
      int len = ack[0]; /* Length byte */
      /* Copy the ack frame to the read-buf */
      add_packet_to_buf(ack + 1, len - 2, ack_info->rssi, ack_info->lqi, ack_info->timestamp); /* -2 for length byte and FCS */

      /* Process the ack frame if needed */
      esp_ieee802154_receive_handle_done(ack);
    } else {
      /* Should check if we expected ACK or not... */
        tx_status = RADIO_TX_OK;
        ESP_EARLY_LOGI("TX", "No Ack received");
    }
}
/*---------------------------------------------------------------------------*/
static IRAM_ATTR void tx_fail_cb(const uint8_t *psdu,
                                 esp_ieee802154_tx_error_t err)
{
    radio_state = RADIO_STATE_IDLE;
    tx_status = RADIO_TX_ERR;
    ESP_EARLY_LOGE("TX", "Tx failed: err=%d", err);
}
/*---------------------------------------------------------------------------*/
void esp_ieee802154_receive_failed(uint16_t error) { 
  /* This function can be overridden by the application to handle receive errors */
  ESP_EARLY_LOGE(TAG, "Receive failed with error: %u", error);
  radio_state = RADIO_STATE_IDLE;
}


static int
init(void)
{

  /* 1. Register callbacks */
    esp_ieee802154_event_cb_list_t cbs = {
        .tx_done_cb   = tx_done_cb,
        .tx_failed_cb = tx_fail_cb,
        .ed_done_cb = ed_done_cb,
        .rx_done_cb = rx_done_cb,
        .rx_sfd_done_cb = rx_sfd_done_cb,
    };
  ESP_ERROR_CHECK(esp_ieee802154_event_callback_list_register(cbs)); 

  esp_ieee802154_enable();
  esp_ieee802154_set_panid(IEEE802154_CONF_PANID);
  esp_ieee802154_set_channel(IEEE802154_DEFAULT_CHANNEL);
  /* Set the link layer address (8 bytes)*/
  uint8_t eui64[8];
  esp_ieee802154_get_extended_address(eui64);
  ESP_LOGI(TAG, "Extended address (before setting): ");
  ESP_LOG_BUFFER_HEXDUMP(TAG, eui64, sizeof(eui64), ESP_LOG_INFO);

  /* --- NEW: extended address in little-endian ----------------------------- */
  uint8_t eui64_le[8];
  for(int i = 0; i < 8; i++) eui64_le[i] = linkaddr_node_addr.u8[7 - i];
  ESP_ERROR_CHECK(esp_ieee802154_set_extended_address(eui64_le)); /* <-- FIX #2 */

  esp_ieee802154_get_extended_address(eui64);
  ESP_LOGI(TAG, "Extended address (after setting): ");
  ESP_LOG_BUFFER_HEXDUMP(TAG, eui64, sizeof(eui64), ESP_LOG_INFO);

  esp_ieee802154_receive();
  /* 4. Optionally switch on promiscuous mode to ignore PAN filters */
  ESP_ERROR_CHECK(esp_ieee802154_set_promiscuous(false));
  ESP_ERROR_CHECK(esp_ieee802154_set_rx_when_idle(true));
  ESP_ERROR_CHECK(esp_ieee802154_set_coordinator(false));

  /* 5. Start the Contiki process */
  process_start(&esp_ieee802154_process, NULL);
  return 0;
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(esp_ieee802154_process, ev, data)
{
  PROCESS_BEGIN();
  rx_pending = false;  /* reset before input() to avoid re-entrancy */
  ESP_LOGI(TAG, "ESP32 Radio Process starting...");

  while(1) {
    PROCESS_YIELD_UNTIL(rx_pending);
    packetbuf_copyfrom(rx_buf, rx_len);
    rx_pending = false;  /* reset before input() to avoid re-entrancy */
    ESP_LOGI(TAG, "RX: %u bytes", rx_len);
    ESP_LOG_BUFFER_HEXDUMP(TAG, rx_buf, rx_len, ESP_LOG_DEBUG);

    packetbuf_set_attr(PACKETBUF_ATTR_RSSI, rx_rssi);
    NETSTACK_MAC.input();

    rx_pending = false;
  }
  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
/* TX path (blocking for now)                                                 */
static uint8_t tx_buf[RX_BUF_LEN];
static int
prepare(const void *payload, unsigned short len)
{
  if(len > RX_BUF_LEN) return RADIO_TX_ERR;
  memcpy(tx_buf + 1, payload, len);
  tx_buf[0] = len + 2; /* +2 for length byte and FCS (?)*/

  return RADIO_TX_OK;
}
static int
transmit(unsigned short len)
{
  ESP_LOGI(TAG, "TX: %u bytes", len);
  ESP_LOG_BUFFER_HEXDUMP(TAG, rx_buf, len, ESP_LOG_DEBUG);

  if (radio_state != RADIO_STATE_IDLE) {
    ESP_LOGE(TAG, "Radio is not idle, cannot transmit %s", get_state_string());
    return RADIO_TX_ERR;
  }
  radio_state = RADIO_STATE_TRANSMITTING;
  esp_ieee802154_transmit(tx_buf, false /* CSMA off for now */);

  /* Wait for the TX done callback to be called */
  while(radio_state == RADIO_STATE_TRANSMITTING) {
    vTaskDelay(pdMS_TO_TICKS(1));  /* Yield to other tasks */
  }

  return tx_status; /* Return the status of the transmission */
}

static int
send(const void *payload, unsigned short len)
{
  int r = prepare(payload, len);
  return (r == RADIO_TX_OK) ? transmit(len) : r;
}
/*---------------------------------------------------------------------------*/
/* RX helpers used by CSMA/LLSEC                                              */
static int
read(void *buf, unsigned short size)         
{ 
  int len;
  if(size < rx_len) {
    ESP_LOGE(TAG, "Buffer too small: %u < %u", size, rx_len);
    return 0;
  }
  len = get_packet_from_buf(buf, size);
  if(!len) {
    ESP_LOGE(TAG, "Failed to get packet from buffer");
    return 0;
  } else {
    ESP_LOGI(TAG, "Read %u bytes from RX buffer", len);
  }
  return len;
}


static int
channel_clear(void)                           
{
  /* Assume clear when not transmitting */
  return (get_radio_state() != RADIO_STATE_TRANSMITTING);
}

static int
receiving_packet(void)
{
  /* Return true if we are currently likely receiving a packet */
  return (get_radio_state() == RADIO_STATE_RECEIVING);
}

static int
pending_packet(void)
{
   return rx_pending;
}
/*---------------------------------------------------------------------------*/
static int on(void)
{
  radio_state = RADIO_STATE_IDLE;
  esp_ieee802154_receive(); 
  return 0; /* success */
}
/*---------------------------------------------------------------------------*/
static int off(void) 
{ 
  radio_state = RADIO_STATE_OFF;
  esp_ieee802154_sleep();
  return 0;
}
/*---------------------------------------------------------------------------*/
/* Parameter getters/setters (only what we support)                           */
static radio_result_t
get_value(radio_param_t p, radio_value_t *v)
{
  switch(p) {
  case RADIO_PARAM_CHANNEL: 
    *v = esp_ieee802154_get_channel();
    return RADIO_RESULT_OK;
  case RADIO_PARAM_PAN_ID:
    *v = esp_ieee802154_get_panid();
    return RADIO_RESULT_OK;
  case RADIO_CONST_CHANNEL_MIN:
    *v = 11;  /* IEEE 802.15.4-2015, Table 6-2 */
    return RADIO_RESULT_OK;
  case RADIO_CONST_CHANNEL_MAX:
    *v = 26;  /* IEEE 802.15.4-2015, Table 6-2 */
    return RADIO_RESULT_OK;
  case RADIO_CONST_MAX_PAYLOAD_LEN:
    /* MAX - Checksum */
    *v = 127 - 2; /* IEEE 802.15.4-2015, Table 6-3 */
    return RADIO_RESULT_OK;
  default: return RADIO_RESULT_NOT_SUPPORTED;
  }
}

static radio_result_t
set_value(radio_param_t p, radio_value_t v)
{
  switch(p) {
  case RADIO_PARAM_CHANNEL:
    return esp_ieee802154_set_channel(v) ? RADIO_RESULT_OK : RADIO_RESULT_ERROR;
  case RADIO_PARAM_PAN_ID:
    return esp_ieee802154_set_panid(v) ? RADIO_RESULT_OK : RADIO_RESULT_ERROR;
  default: return RADIO_RESULT_NOT_SUPPORTED;
  }
}
/*---------------------------------------------------------------------------*/

const struct radio_driver esp32c6_radio_driver = {
  init, prepare, transmit, send, read,
  channel_clear, receiving_packet, pending_packet,
  on, off, get_value, set_value, NULL, NULL
};
/*---------------------------------------------------------------------------*/
