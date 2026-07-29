#pragma once
// Reports.cpp instantiates a WiFiClient purely to hand to HTTPClient::begin().
// Nothing is ever read from it natively, so an empty type is enough.
class WiFiClient {};
