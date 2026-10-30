/* Test-only durable event ring. No credentials or location data.
 * SPDX-License-Identifier: Apache-2.0 */
#include "test_debug.h"
#if FRAMENAV_TEST_DEBUG
#include <stdio.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "driver/usb_serial_jtag.h"
#include "soc/usb_serial_jtag_struct.h"
#define SLOTS 128U
#define MAGIC 0x464e4431U
typedef struct {
  uint32_t magic,seq,boot,ms,event,source;
  int32_t a,b,c,d;
} record_t;
static record_t history[SLOTS];
static QueueHandle_t queue;
static nvs_handle_t handle;
static uint32_t sequence,boot_id;
static _Atomic unsigned pending,dropped;
static const char *TAG="test_debug";
static const char *name(unsigned e) {
  static const char *names[]={"?","BOOT","SOURCE","AP_START",
    "WIFI_STOP","WIFI_ERROR"};
  return e<sizeof(names)/sizeof(names[0])?names[e]:"?";
}
static void print_record(const record_t *r) {
  ESP_LOGI(TAG,"DLOG seq=%lu boot=%lu ms=%lu event=%s source=%lu a=%ld b=%ld c=%ld d=%ld",
    (unsigned long)r->seq,(unsigned long)r->boot,(unsigned long)r->ms,name(r->event),
    (unsigned long)r->source,(long)r->a,(long)r->b,(long)r->c,(long)r->d);
}
static void writer(void *unused) {
  (void)unused;
  vTaskDelay(pdMS_TO_TICKS(1500));
  if(usb_serial_jtag_is_connected()) {
    ESP_LOGI(TAG,"Saved history BEGIN (previous boots), latest=%lu",(unsigned long)sequence);
    uint32_t first=sequence>=SLOTS?sequence-SLOTS+1:1;
    for(uint32_t n=first;n<=sequence;n++) {
      record_t *r=&history[n%SLOTS];if(r->magic==MAGIC && r->seq==n)print_record(r);
    }
    ESP_LOGI(TAG,"Saved history END; source bits: SOF=1 DP=2 DM=4");
  }
  record_t r;
  for(;;) {
    if(xQueueReceive(queue,&r,portMAX_DELAY)!=pdTRUE)continue;
    r.seq=++sequence;char key[12];snprintf(key,sizeof(key),"e%03u",(unsigned)(r.seq%SLOTS));
    esp_err_t err=nvs_set_blob(handle,key,&r,sizeof(r));
    if(err==ESP_OK)err=nvs_commit(handle);
    if(err!=ESP_OK)ESP_LOGE(TAG,"debug persist failed: %s",esp_err_to_name(err));
    if(usb_serial_jtag_is_connected())print_record(&r);
    atomic_fetch_sub(&pending,1);
    unsigned lost=atomic_exchange(&dropped,0);
    if(lost)ESP_LOGW(TAG,"debug queue dropped %u events",lost);
  }
}
void test_debug_event(unsigned event,int32_t a,int32_t b,int32_t c,int32_t d) {
  if(!queue)return;
  uint32_t phy=USB_SERIAL_JTAG.test.val;
  record_t r={.magic=MAGIC,.boot=boot_id,.ms=(uint32_t)(esp_timer_get_time()/1000),
    .event=event,.source=(usb_serial_jtag_is_connected()?1U:0U)|
                        (((phy>>5)&1U)<<1)|(((phy>>6)&1U)<<2),
    .a=a,.b=b,.c=c,.d=d};
  atomic_fetch_add(&pending,1);
  if(xQueueSend(queue,&r,0)!=pdTRUE) {
    atomic_fetch_sub(&pending,1);atomic_fetch_add(&dropped,1);
  }
}
void test_debug_flush(void) {
  int64_t end=esp_timer_get_time()+2000000LL;
  while(atomic_load(&pending) && esp_timer_get_time()<end)vTaskDelay(pdMS_TO_TICKS(10));
}
void test_debug_start(void) {
  esp_err_t err=nvs_flash_init_partition("debuglog");
  if(err==ESP_OK)err=nvs_open_from_partition("debuglog","events",NVS_READWRITE,&handle);
  if(err!=ESP_OK){ESP_LOGE(TAG,"debug partition unavailable: %s (not erased)",esp_err_to_name(err));return;}
  for(unsigned i=0;i<SLOTS;i++) {
    char key[12];snprintf(key,sizeof(key),"e%03u",i);size_t len=sizeof(record_t);
    if(nvs_get_blob(handle,key,&history[i],&len)==ESP_OK && len==sizeof(record_t) &&
       history[i].magic==MAGIC && history[i].seq>sequence)sequence=history[i].seq;
  }
  boot_id=sequence+1;
  queue=xQueueCreate(32,sizeof(record_t));
  if(!queue){nvs_close(handle);return;}
  if(xTaskCreate(writer,"debug_log",4096,NULL,1,NULL)!=pdPASS) {
    vQueueDelete(queue);queue=NULL;nvs_close(handle);return;
  }
  ESP_LOGW(TAG,"TEST DEBUG enabled: independent 64KiB partition, last 128 events; not a power benchmark");
  test_debug_event(DBG_BOOT,esp_reset_reason(),0,0,0);
}
#endif
