#pragma once
#include <sys/system_properties.h>
#include <cstring>

// Explicit ADB-only controls for same-build device comparisons. Defaults are inert.
inline bool diagnostic_property(const char *name, const char *expected) {
  char value[PROP_VALUE_MAX]{};
  return __system_property_get(name, value) > 0 && std::strcmp(value, expected) == 0;
}
