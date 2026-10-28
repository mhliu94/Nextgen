// Embedded web assets generated at build time from web/static (cmake/embed_assets.cmake).
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ng::assets {

struct Asset {
    const char* path;           // relative path, e.g. "index.html" or "js/app.js"
    const unsigned char* data;  // NUL-terminated bytes
    std::size_t size;
};

extern const Asset kAssets[];
extern const std::size_t kAssetCount;

inline const Asset* find(std::string_view path) {
    for (std::size_t i = 0; i < kAssetCount; ++i) {
        if (path == kAssets[i].path) return &kAssets[i];
    }
    return nullptr;
}

inline std::string content_type_for(std::string_view path) {
    auto ends = [&](std::string_view suffix) {
        return path.size() >= suffix.size() && path.substr(path.size() - suffix.size()) == suffix;
    };
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".js") || ends(".mjs")) return "application/javascript; charset=utf-8";
    if (ends(".css")) return "text/css; charset=utf-8";
    if (ends(".json")) return "application/json; charset=utf-8";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".png")) return "image/png";
    if (ends(".ico")) return "image/x-icon";
    if (ends(".woff2")) return "font/woff2";
    if (ends(".webmanifest")) return "application/manifest+json";
    if (ends(".txt")) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

}  // namespace ng::assets
