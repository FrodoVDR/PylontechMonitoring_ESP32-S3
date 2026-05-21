#pragma once
#include "api_core.h"
#include "../py_uart.h"
#include "../py_scheduler.h"

extern PyUart py_uart;
extern PyScheduler py_scheduler;

inline void registerConsoleAPI() {

    apiGet("/req", []() {
        py_scheduler.enqueue(server.arg("code"));
        apiText("OK");
    });

    apiGet("/api/lastframe", []() {
        unsigned long start = millis();
        while (!py_uart.hasFrame()) {
            if (millis() - start > 2000)
                return apiText("TIMEOUT");
            vTaskDelay(1);
        }
        apiText(py_uart.getFrame());
    });
}
