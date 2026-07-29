#pragma once
// Temperatures.h declares "extern OneWire g_oneWireBus1/2". Temperatures.cpp
// constructs them with a pin number (never otherwise called - DallasTemperature
// is the type whose methods actually get exercised), so a pin-number
// constructor is enough; no other behaviour is needed.
class OneWire {
public:
  OneWire(int /*pin*/) {}
};
