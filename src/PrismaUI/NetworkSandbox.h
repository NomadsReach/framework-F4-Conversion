#pragma once

#include <string>

#include "URLWhitelist.h"

namespace PrismaUI::NetworkSandbox {

inline std::string BuildNetworkBlockScript() {
    std::string csp =
        "default-src 'self' 'unsafe-inline' 'unsafe-eval' file: data: blob:; "
        "connect-src 'none'; " +
        URLWhitelist::GenerateScriptSrcDirective() + " " + URLWhitelist::GenerateImgSrcDirective() + " " +
        URLWhitelist::GenerateStyleSrcDirective() + " " + URLWhitelist::GenerateFontSrcDirective() +
        " media-src 'none'; object-src 'none'; frame-src 'none'; worker-src 'none'; form-action 'none';";

    std::string script = R"js(
(function(){
'use strict';
var DEF = Object.defineProperty.bind(Object);
var DEAD = { value: undefined, writable: false, configurable: false, enumerable: true };
var BLOCKED = ['fetch', 'XMLHttpRequest', 'WebSocket', 'EventSource', 'Worker', 'SharedWorker'];
for (var i = 0; i < BLOCKED.length; ++i) {
    try { DEF(window, BLOCKED[i], DEAD); } catch(e) {}
}
try {
    DEF(navigator, 'sendBeacon', { value: function(){ return false; }, writable: false, configurable: false });
} catch(e) {}
try {
    DEF(navigator, 'serviceWorker', DEAD);
} catch(e) {}
try {
    var meta = document.createElement('meta');
    meta.setAttribute('http-equiv', 'Content-Security-Policy');
    meta.setAttribute('content', `)js";
    script += csp;
    script += R"js(`);
    if (document.head) {
        document.head.insertBefore(meta, document.head.firstChild);
    }
} catch(e) {}
})();
)js";
    return script;
}

}
