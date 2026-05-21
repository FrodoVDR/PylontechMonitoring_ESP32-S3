#pragma once
#include "api_core.h"
#include "../py_uart.h"

extern PyUart py_uart;

inline void registerFramedumpApi() {
    apiGet("/api/framedump", []() {
        apiText(py_uart.getLastRawFrame());
    });
}
