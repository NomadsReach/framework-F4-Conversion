#pragma once

#include "URLWhitelist.h"

namespace PrismaUI::NetworkSandbox {

constexpr const char* kNetworkBlockScript = R"js(
(function(){
'use strict';
var DEF = Object.defineProperty.bind(Object);
var DEAD = { value: undefined, writable: false, configurable: false, enumerable: true };

// ── Kill JS-accessible network constructors and functions ──────────────────
var BLOCKED = [
    'fetch',
    'XMLHttpRequest',
    'WebSocket',
    'EventSource',
    'Worker',
    'SharedWorker'
];
for (var i = 0; i < BLOCKED.length; ++i) {
    try { DEF(window, BLOCKED[i], DEAD); } catch(e) {}
}

// navigator APIs
try {
    DEF(navigator, 'sendBeacon', { value: function(){ return false; }, writable: false, configurable: false });
} catch(e) {}
try {
    DEF(navigator, 'serviceWorker', DEAD);
} catch(e) {}

// ── Inject Content-Security-Policy meta (defense-in-depth) ────────────────
// Blocks HTML-attribute-initiated network loads EXCEPT whitelisted domains
// See URLWhitelist.h to configure allowed domains
try {
    var meta = document.createElement('meta');
    meta.setAttribute('http-equiv', 'Content-Security-Policy');
    meta.setAttribute('content',
        "default-src 'self' 'unsafe-inline' 'unsafe-eval' file: data: blob:; " +
        "connect-src 'none'; " +
        "script-src 'self' 'unsafe-inline' 'unsafe-eval' file: data: https://static.wikia.nocookie.net https://cdn.jsdelivr.net https://fonts.googleapis.com https://youtube.com https://youtu.be https://nexusmods.com; " +
        "img-src 'self' data: blob: file: https://static.wikia.nocookie.net https://cdn.jsdelivr.net https://fonts.googleapis.com https://youtube.com https://youtu.be https://nexusmods.com; " +
        "style-src 'self' 'unsafe-inline' file: data: https://static.wikia.nocookie.net https://cdn.jsdelivr.net https://fonts.googleapis.com https://youtube.com https://youtu.be https://nexusmods.com; " +
        "font-src 'self' data: file: https://static.wikia.nocookie.net https://cdn.jsdelivr.net https://fonts.googleapis.com https://youtube.com https://youtu.be https://nexusmods.com; " +
        "media-src 'none'; " +
        "object-src 'none'; " +
        "frame-src 'none'; " +
        "worker-src 'none'; " +
        "form-action 'none';"
    );
    if (document.head) {
        document.head.insertBefore(meta, document.head.firstChild);
    }
} catch(e) {}
})();
)js";

} // namespace PrismaUI::NetworkSandbox
