#pragma once
// Temperatures.h declares "extern OneWire g_oneWireBus1/2" - nothing under
// native test compiles Temperatures.cpp or calls into the real OneWire bus,
// so an empty type is enough to satisfy those declarations.
class OneWire {};
