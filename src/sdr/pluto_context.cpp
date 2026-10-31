#include "pluto_context.h"

#include <iio.h>

#include <cstring>
#include <vector>

namespace mctrx {
namespace {

iio_context* tryUri(const std::string& uri)
{
    if (uri.empty()) return nullptr;
    return iio_create_context_from_uri(uri.c_str());
}

} // namespace

iio_context* openPlutoContext(const std::string& requested,
                              std::string& usedUri,
                              std::string& err)
{
    if (iio_context* ctx = tryUri(requested)) {
        usedUri = requested;
        return ctx;
    }

    iio_scan_context* scan = iio_create_scan_context(nullptr, 0);
    if (!scan) {
        err = "no IIO scan context and requested URI failed: " + requested;
        return nullptr;
    }

    iio_context_info** list = nullptr;
    const ssize_t count = iio_scan_context_get_info_list(scan, &list);
    std::vector<std::string> usbUris;
    std::vector<std::string> ipUris;
    if (count > 0 && list) {
        for (ssize_t i = 0; i < count; ++i) {
            const char* u = iio_context_info_get_uri(list[i]);
            if (!u) continue;
            const std::string s(u);
            if (s.rfind("usb:", 0) == 0) usbUris.push_back(s);
            else ipUris.push_back(s);
        }
        iio_context_info_list_free(list);
    }
    iio_scan_context_destroy(scan);

    for (const auto& u : usbUris) {
        if (iio_context* ctx = tryUri(u)) {
            usedUri = u;
            return ctx;
        }
    }
    for (const auto& u : ipUris) {
        if (iio_context* ctx = tryUri(u)) {
            usedUri = u;
            return ctx;
        }
    }

    err = "Pluto not found";
    if (!requested.empty()) err += " (requested " + requested + ")";
    err += ". Check USB/IP connection and close other Pluto applications.";
    return nullptr;
}

} // namespace mctrx
