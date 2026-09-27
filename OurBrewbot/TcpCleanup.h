/*
 * Copyright 2026 Sean Cardus
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
/*
 * TcpCleanup.h — Clear out closed TCP connections waiting in TIME_WAIT,
 * to avoid a double free in the ESP8266 core's lwIP. See TcpCleanup.cpp.
 */

#include <stdint.h>

// Remove every TCP connection in TIME_WAIT. Call once per loop() pass.
// Returns how many were removed.
uint32_t tcpClearTimeWait();

// Connections removed since the last call - for the 10-minute health log
uint32_t tcpTakeClearedCount();
