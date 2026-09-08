/* Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include "motion_service.h"
#include "test_debug.h"
#if FRAMENAV_MOTION
#include "motion_policy.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/usb_serial_jtag.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
static const char *TAG="motion";
static SemaphoreHandle_t mutex;
static motion_policy_t policy;
static bool healthy, paused, circle;
static uint8_t address;
static esp_err_t rd(uint8_t reg,uint8_t *data,size_t n) {
  esp_err_t e=i2c_master_write_read_device(I2C_NUM_0,address,&reg,1,data,n,pdMS_TO_TICKS(80));
  if(e!=ESP_OK)test_debug_event(DBG_SENSOR_ERROR,reg,e,address,0);
  return e;
}
static bool wr(uint8_t reg,uint8_t v) {
  uint8_t b[]={reg,v};
  esp_err_t e=i2c_master_write_to_device(I2C_NUM_0,address,b,2,pdMS_TO_TICKS(80));
  if(e!=ESP_OK)test_debug_event(DBG_SENSOR_ERROR,reg,e,address,1);
  vTaskDelay(pdMS_TO_TICKS(10));return e==ESP_OK;
}
static bool read_equals(uint8_t reg,uint8_t expected) {
  uint8_t value=0;return rd(reg,&value,1)==ESP_OK && value==expected;
}
static bool setup(void) {
  for(address=0x68;address<=0x69;address++) if(read_equals(0,0xd1))break;
  if(address>0x69)return false;
  if(!wr(0x7e,0xb6))return false;
  vTaskDelay(pdMS_TO_TICKS(100));
  // BMI160 low-power 50Hz, undersampling, four-sample averaging, +/-4g.
  if(!wr(0x40,0xa7)||!wr(0x41,5)||!wr(0x7e,0x12))return false;
  vTaskDelay(pdMS_TO_TICKS(100));
  if(!read_equals(3,0x20)||!read_equals(0x40,0xa7)||!read_equals(0x41,5))return false;
  gpio_config_t input={.pin_bit_mask=1ULL<<GPIO_NUM_0,.mode=GPIO_MODE_INPUT,
                      .pull_down_en=GPIO_PULLDOWN_ENABLE};
  if(gpio_config(&input)!=ESP_OK)return false;
  if(!wr(0x53,8)||gpio_get_level(GPIO_NUM_0)!=1 ||
     !wr(0x53,10)||gpio_get_level(GPIO_NUM_0)!=0)return false;
  // Sensitive any-motion latch is ONLY a wake source, never a WiFi gesture.
  if(!wr(0x5f,1)||!wr(0x60,4)||!wr(0x55,4)||!wr(0x54,15)||!wr(0x50,7))return false;
  return read_equals(0x53,10)&&read_equals(0x55,4)&&read_equals(0x50,7);
}
static void task(void *unused) {
  (void)unused;
  xSemaphoreTake(mutex,portMAX_DELAY);
  healthy=setup();motion_policy_init(&policy,esp_timer_get_time());
  test_debug_event(DBG_SENSOR_INIT,healthy,address,0,0);
  ESP_LOGI(TAG,"BMI160 low-power 50Hz INT1/D0=%s; idle=300s; circle experimental",
           healthy?"READY":"FAILED (deep sleep disabled)");
  xSemaphoreGive(mutex);
  int64_t report=0, trace=0;
#if FRAMENAV_TEST_DEBUG
  int64_t debug_report=0;
  unsigned samples=0;
#endif
  for(;;) {
    xSemaphoreTake(mutex,portMAX_DELAY);
    if(healthy&&!paused) {
      uint8_t status=0,raw[6];
      int64_t now=esp_timer_get_time();
      if(rd(0x1b,&status,1)!=ESP_OK)healthy=false;
      else if(status&0x80) {
        if(rd(0x12,raw,6)!=ESP_OK)healthy=false;
        else {
#if FRAMENAV_TEST_DEBUG
          samples++;
#endif
          float x=(int16_t)(raw[0]|raw[1]<<8)/8192.0f;
          float y=(int16_t)(raw[2]|raw[3]<<8)/8192.0f;
          float z=(int16_t)(raw[4]|raw[5]<<8)/8192.0f;
          bool was_capturing=policy.capturing;
          int64_t processing_start=esp_timer_get_time();
          bool accepted=motion_policy_sample(&policy,now,x,y,z);
          if(!was_capturing && policy.capturing)
            test_debug_event(DBG_CAPTURE_START,policy.armed,0,0,0);
          if(was_capturing && !policy.capturing)
            test_debug_event(DBG_CAPTURE_END,accepted,policy.count,policy.moving_count,(now-policy.started)/1000);
          if(!was_capturing && policy.capturing) ESP_LOGI(TAG,"Gesture capture started");
          if(was_capturing && !policy.capturing)
            ESP_LOGI(TAG,"Gesture finished: accepted=%d points=%u moving_points=%u duration_ms=%lld processing_ms=%lld",
                     accepted,policy.count,policy.moving_count,(long long)((now-policy.started)/1000),
                     (long long)((esp_timer_get_time()-processing_start)/1000));
          if(accepted) {
            circle=true;ESP_LOGI(TAG,"CIRCLE accepted; cooldown=60s");
          }
          // Bounded USB-only diagnostics for physical gesture calibration.
          if(usb_serial_jtag_is_connected() && now<180000000LL && now-trace>=20000) {
            trace=now;
            ESP_LOGI(TAG,"sample x=%.3fg y=%.3fg z=%.3fg armed=%d capture=%d",x,y,z,policy.armed,policy.capturing);
          }
        }
      }
      if(gpio_get_level(GPIO_NUM_0)) {
        policy.last_motion=now;
        if(!wr(0x7e,0xb1))healthy=false;
      }
      if(now-report>=30000000LL) {
        ESP_LOGI(TAG,"status sensor=%d quiet=%llds capturing=%d",
                 healthy,(long long)((now-policy.last_motion)/1000000),policy.capturing);
        report=now;
      }
    }
#if FRAMENAV_TEST_DEBUG
    int64_t debug_now=esp_timer_get_time();
    if(debug_now-debug_report>=30000000LL) {
      test_debug_event(DBG_SENSOR_STATE,healthy|(paused<<1)|(policy.initialized<<2)|
        (policy.armed<<3)|(policy.capturing<<4)|(gpio_get_level(GPIO_NUM_0)<<5),
        policy.cooldown_until>debug_now?(policy.cooldown_until-debug_now)/1000000:0,
        samples,(debug_now-policy.quiet_since)/1000000);
      samples=0;debug_report=debug_now;
    }
#endif
    xSemaphoreGive(mutex);vTaskDelay(pdMS_TO_TICKS(20));
  }
}
void motion_service_start(void) {
  mutex=xSemaphoreCreateMutex();if(!mutex)return;
  i2c_config_t c={.mode=I2C_MODE_MASTER,.sda_io_num=20,.scl_io_num=18,
    .sda_pullup_en=GPIO_PULLUP_ENABLE,.scl_pullup_en=GPIO_PULLUP_ENABLE,
    .master.clk_speed=100000};
  if(i2c_param_config(I2C_NUM_0,&c)!=ESP_OK ||
     i2c_driver_install(I2C_NUM_0,c.mode,0,0,0)!=ESP_OK)return;
  if(xTaskCreate(task,"motion",4096,NULL,3,NULL)!=pdPASS) {
    i2c_driver_delete(I2C_NUM_0);ESP_LOGE(TAG,"task allocation failed");
  }
}
bool motion_service_status(int64_t *last) {
  if(!mutex||xSemaphoreTake(mutex,pdMS_TO_TICKS(40))!=pdTRUE)return false;
  *last=policy.last_motion;
  bool ok=healthy&&policy.initialized&&(paused||esp_timer_get_time()-policy.last_sample<500000);
  xSemaphoreGive(mutex);return ok;
}
bool motion_service_take_circle(void) {
  if(!mutex||xSemaphoreTake(mutex,pdMS_TO_TICKS(40))!=pdTRUE)return false;
  bool result=circle;circle=false;xSemaphoreGive(mutex);return result;
}
bool motion_service_arm_sleep(void) {
  if(!mutex||xSemaphoreTake(mutex,pdMS_TO_TICKS(200))!=pdTRUE)return false;
  bool ok=healthy && policy.initialized && esp_timer_get_time()-policy.last_motion>=300000000LL;
  if(ok)ok=wr(0x7e,0xb1)&&read_equals(3,0x20)&&read_equals(0x50,7)&&
           read_equals(0x53,10)&&read_equals(0x54,15)&&read_equals(0x55,4);
  if(ok)ok=gpio_get_level(GPIO_NUM_0)==0;
  if(ok)paused=true;
  xSemaphoreGive(mutex);return ok;
}
void motion_service_cancel_sleep(void) {
  if(!mutex)return;
  xSemaphoreTake(mutex,portMAX_DELAY);paused=false;
  policy.last_motion=esp_timer_get_time();xSemaphoreGive(mutex);
}
void motion_service_boot(void) {
  // ESP32-C6 uses per-pad holds in deep sleep, not the older global API.
  gpio_hold_dis((gpio_num_t)GPS_UART_TX_PIN);gpio_hold_dis(GPIO_NUM_15);
  ESP_LOGI(TAG,"wake cause=%d ext1=%llx",esp_sleep_get_wakeup_cause(),
           (unsigned long long)esp_sleep_get_ext1_wakeup_status());
  nvs_handle_t h;
  if (nvs_open("motion_diag",NVS_READWRITE,&h)==ESP_OK) {
    uint32_t wakes=0,entries=0;
    nvs_get_u32(h,"wakes",&wakes); nvs_get_u32(h,"entries",&entries);
    if(esp_sleep_get_wakeup_cause()==ESP_SLEEP_WAKEUP_EXT1) {
      nvs_set_u32(h,"wakes",++wakes); nvs_commit(h);
    }
    ESP_LOGI(TAG,"Persistent deep-sleep entries=%lu motion-wakes=%lu",
             (unsigned long)entries,(unsigned long)wakes);
    nvs_close(h);
  }
}
void motion_service_enter_sleep(void) {
  // UART TX stays idle HIGH so entering sleep cannot wake the GNSS receiver.
  gpio_hold_en((gpio_num_t)GPS_UART_TX_PIN);
  gpio_reset_pin(GPIO_NUM_15);
  gpio_set_direction(GPIO_NUM_15,GPIO_MODE_OUTPUT);gpio_set_level(GPIO_NUM_15,1);
  gpio_hold_en(GPIO_NUM_15);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(1ULL<<GPIO_NUM_0,ESP_EXT1_WAKEUP_ANY_HIGH));
  nvs_handle_t h;
  if(nvs_open("motion_diag",NVS_READWRITE,&h)==ESP_OK) {
    uint32_t count=0;nvs_get_u32(h,"entries",&count);
    nvs_set_u32(h,"entries",count+1);nvs_commit(h);nvs_close(h);
  }
  if (usb_serial_jtag_is_connected()) esp_restart();
  ESP_LOGI(TAG,"DEEP SLEEP: no timers; wake on BMI160 INT1/D0");
  test_debug_event(DBG_SLEEP_ENTER,0,0,0,0);
  test_debug_flush();
  esp_deep_sleep_start();
}
#else
void motion_service_start(void){}
bool motion_service_status(int64_t *last){(void)last;return false;}
bool motion_service_take_circle(void){return false;}
bool motion_service_arm_sleep(void){return false;}
void motion_service_cancel_sleep(void){}
void motion_service_boot(void){}
void motion_service_enter_sleep(void){}
#endif
