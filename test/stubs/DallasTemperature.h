#pragma once
// Temperatures.h declares "extern DallasTemperature g_sensors1/2" and uses
// DeviceAddress in a couple of signatures - nothing under native test
// compiles Temperatures.cpp or talks to real DS18B20 hardware, so an empty
// type plus the address typedef is enough to satisfy those declarations.
class DallasTemperature {};
typedef uint8_t DeviceAddress[8];
