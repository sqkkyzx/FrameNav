/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ALPHALOC_POWER_SOURCE_PROBE_H
#define ALPHALOC_POWER_SOURCE_PROBE_H

// One-shot diagnostic used to compare USB data-host, MI-shoe adapter and
// charger/power-bank electrical signatures. It does not control Wi-Fi.
void power_source_probe_start(void);

#endif
