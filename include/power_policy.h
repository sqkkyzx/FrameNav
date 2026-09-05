/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ALPHALOC_POWER_POLICY_H
#define ALPHALOC_POWER_POLICY_H

// Opt-in only: the known-good debug environments retain their old behavior.
#ifndef ALPHALOC_POWER_SAVE
#define ALPHALOC_POWER_SAVE 0
#endif

#if ALPHALOC_POWER_SAVE
#define ALPHALOC_PM_MAX_FREQ_MHZ 80
#define ALPHALOC_GNSS_STATUS_PERIOD_US 30000000LL
#define ALPHALOC_LOCATION_INTERVAL_MS 2000
#else
#define ALPHALOC_PM_MAX_FREQ_MHZ 160
#define ALPHALOC_GNSS_STATUS_PERIOD_US 5000000LL
#define ALPHALOC_LOCATION_INTERVAL_MS 3000 // Preserve the real-GNSS debug rollback.
#endif
#define ALPHALOC_PM_MIN_FREQ_MHZ 40

// BLE scan units are 0.625 ms, independent of the location publisher interval.
#define ALPHALOC_SCAN_BOOT_PERIOD_US 30000000LL // Since MCU boot, not disconnect.
#define ALPHALOC_SCAN_PAIRING_INTERVAL 0x0010   // 10 ms: no stored bonds only.
#define ALPHALOC_SCAN_PAIRING_WINDOW 0x0010     // 10 ms: continuous discovery.
#define ALPHALOC_SCAN_BONDED_BOOT_INTERVAL 0x0640 // 1000 ms, first 30 seconds.
#define ALPHALOC_SCAN_BONDED_BOOT_WINDOW 0x01E0   // 300 ms during boot grace.
#define ALPHALOC_SCAN_BONDED_IDLE_INTERVAL 0x12C0 // 3000 ms, thereafter.
#define ALPHALOC_SCAN_BONDED_IDLE_WINDOW 0x00A0   // 100 ms: 1/30 scheduled RX duty.

#endif
