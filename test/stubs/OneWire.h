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
// Temperatures.h declares "extern OneWire g_oneWireBus1/2". Temperatures.cpp
// constructs them with a pin number (never otherwise called - DallasTemperature
// is the type whose methods actually get exercised), so a pin-number
// constructor is enough; no other behaviour is needed.
class OneWire {
public:
  OneWire(int /*pin*/) {}
};
