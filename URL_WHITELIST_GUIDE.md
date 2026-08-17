# PrismaUI URL Policy

PrismaUI views are local privileged pages loaded from:

```text
file:///views/
```

Main-frame navigation outside that origin is blocked and returned to the view's original local page. The native/Papyrus bridge is only injected into trusted local main frames.

Network APIs such as `fetch`, `XMLHttpRequest`, `WebSocket`, `EventSource`, workers, service workers, and `sendBeacon` are disabled by the runtime sandbox.

The current passive resource policy is defined in `src/PrismaUI/URLWhitelist.h`. Do not duplicate domain lists elsewhere.

Remote JavaScript is limited to the explicitly configured script allowlist. Remote styles, images, and fonts use separate allowlists.

If a view needs a new remote resource host, update the narrowest applicable allowlist and test the view without weakening `connect-src`, `frame-src`, `worker-src`, `object-src`, or main-frame navigation restrictions.
