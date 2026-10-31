#pragma once

#include <string>

struct iio_context;

namespace mctrx {

// Open the requested Pluto URI. If it is stale or unavailable, scan for a
// Pluto and use the first USB context (or IP context as a fallback).
iio_context* openPlutoContext(const std::string& requested,
                              std::string& usedUri,
                              std::string& err);

} // namespace mctrx
