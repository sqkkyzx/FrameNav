/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "captive_dns.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "captive_dns_packet.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define CAPTIVE_DNS_PORT 53
#define CAPTIVE_DNS_PACKET_MAX 512
#define CAPTIVE_DNS_STOP_TIMEOUT_MS 1200

static const char *TAG = "captive_dns";

struct captive_dns_server {
  volatile bool running;
  volatile int socket_fd;
  uint32_t ipv4_network_order;
  TaskHandle_t task;
  SemaphoreHandle_t stopped;
};

static void captive_dns_task(void *arg) {
  captive_dns_handle_t handle = (captive_dns_handle_t)arg;
  int sock = -1;

  if (!handle->running) {
    goto done;
  }
  sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "Unable to create UDP socket: errno=%d", errno);
    goto done;
  }
  handle->socket_fd = sock;

  struct timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
  (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  struct sockaddr_in bind_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(CAPTIVE_DNS_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
    ESP_LOGE(TAG, "Unable to bind UDP/53: errno=%d", errno);
    goto done;
  }
  ESP_LOGI(TAG, "Wildcard DNS responder started on UDP/53");

  while (handle->running) {
    uint8_t request[CAPTIVE_DNS_PACKET_MAX];
    uint8_t reply[CAPTIVE_DNS_PACKET_MAX];
    struct sockaddr_storage source_addr;
    socklen_t source_len = sizeof(source_addr);
    ssize_t received = recvfrom(sock, request, sizeof(request), 0,
                                (struct sockaddr *)&source_addr, &source_len);
    if (received < 0) {
      if (!handle->running) {
        break;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      ESP_LOGW(TAG, "DNS receive failed: errno=%d", errno);
      break;
    }

    size_t reply_len = captive_dns_build_reply(
        request, (size_t)received, handle->ipv4_network_order, reply,
        sizeof(reply));
    if (reply_len == 0) {
      continue;
    }
    if (sendto(sock, reply, reply_len, 0, (struct sockaddr *)&source_addr,
               source_len) < 0 && handle->running) {
      ESP_LOGW(TAG, "DNS reply failed: errno=%d", errno);
    }
  }

done:
  if (sock >= 0) {
    close(sock);
  }
  handle->socket_fd = -1;
  handle->task = NULL;
  handle->running = false;
  xSemaphoreGive(handle->stopped);
  vTaskDelete(NULL);
}

captive_dns_handle_t captive_dns_start(uint32_t ipv4_network_order) {
  captive_dns_handle_t handle = calloc(1, sizeof(*handle));
  if (!handle) {
    ESP_LOGE(TAG, "Unable to allocate DNS server state");
    return NULL;
  }
  handle->socket_fd = -1;
  handle->ipv4_network_order = ipv4_network_order;
  handle->running = true;
  handle->stopped = xSemaphoreCreateBinary();
  if (!handle->stopped) {
    free(handle);
    return NULL;
  }
  if (xTaskCreate(captive_dns_task, "captive_dns", 4096, handle, 5,
                  &handle->task) != pdPASS) {
    vSemaphoreDelete(handle->stopped);
    free(handle);
    return NULL;
  }
  return handle;
}

void captive_dns_stop(captive_dns_handle_t handle) {
  if (!handle) {
    return;
  }
  handle->running = false;
  int sock = handle->socket_fd;
  if (sock >= 0) {
    (void)shutdown(sock, SHUT_RDWR);
  }
  if (xSemaphoreTake(handle->stopped,
                     pdMS_TO_TICKS(CAPTIVE_DNS_STOP_TIMEOUT_MS)) != pdTRUE) {
    ESP_LOGW(TAG, "DNS task did not stop in time; forcing cleanup");
    if (handle->task) {
      vTaskDelete(handle->task);
    }
    sock = handle->socket_fd;
    if (sock >= 0) {
      close(sock);
      handle->socket_fd = -1;
    }
  }
  vSemaphoreDelete(handle->stopped);
  free(handle);
}
