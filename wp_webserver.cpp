#include "wp_webserver.h"
#include "wp_routes.h"
#include "esp_heap_caps.h"


WebServer server(80);

#include "py_log.h"
extern String webLog;

static void handleApiLog() {
  server.send(200, "text/plain", webLog);
}

// interne Callback‑Pointer
CmdCallback g_cmdCb = nullptr;
StatusCallback g_statusCb = nullptr;

void WebServerModule_begin() {
    registerRoutes();
    server.begin();
}

void WebServerModule_handle() {
    // Processing a web request allocates inside the WebServer + lwIP network
    // stack (connection accept, request parse, response buffers). Doing this
    // when the internal heap is critically low triggers a PANIC/EXCEPTION in
    // the network stack (observed as sporadic crashes at stage nrt:web_loop,
    // heap_min dropping to a few hundred bytes). Mirror the existing API cache
    // heap-floor guard (API_CACHE_MIN_FREE_HEAP): while internal heap is below
    // the floor, defer handling new requests so memory can recover instead of
    // crashing. Normal free heap on this device is ~60 KB, so this only trips
    // during dangerous transient spikes and the client simply retries.
    static const size_t WEB_MIN_FREE_HEAP = 14000;

    size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (freeHeap < WEB_MIN_FREE_HEAP) {
        static unsigned long lastWarn = 0;
        unsigned long now = millis();
        if (now - lastWarn > 5000) {
            lastWarn = now;
            Log(LOG_WARN, String("Web: deferring handleClient, low internal heap=") + freeHeap);
        }
        return;
    }

    server.handleClient();
}

void WebServerModule_setCommandCallback(CmdCallback cb) {
    g_cmdCb = cb;
}

void WebServerModule_setStatusCallback(StatusCallback cb) {
    g_statusCb = cb;
}