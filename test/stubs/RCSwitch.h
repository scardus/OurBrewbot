#pragma once
// SmartPlugs.h declares "extern RCSwitch g_rcSwitch" - nothing under native
// test compiles SmartPlugs.cpp or transmits real RF codes, so an empty type
// is enough to satisfy that declaration.
class RCSwitch {};
