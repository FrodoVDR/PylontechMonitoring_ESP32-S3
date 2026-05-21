#pragma once
#include <WebServer.h>

extern WebServer server;

inline void apiText(const String &txt) {
    server.send(200, "text/plain", txt);
}

inline void apiJson(const String &json) {
    server.send(200, "application/json", json);
}

inline void apiError(int code, const String &msg) {
    server.send(code, "text/plain", msg);
}

template<typename FN>
inline void apiGet(const char *path, FN fn) {
    server.on(path, HTTP_GET, fn);
}

template<typename FN>
inline void apiPost(const char *path, FN fn) {
    server.on(path, HTTP_POST, fn);
}
