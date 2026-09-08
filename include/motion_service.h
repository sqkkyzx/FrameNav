/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
void motion_service_start(void);
bool motion_service_status(int64_t *last_motion);
bool motion_service_take_circle(void);
bool motion_service_arm_sleep(void);
void motion_service_cancel_sleep(void);
void motion_service_boot(void);
void motion_service_enter_sleep(void);
