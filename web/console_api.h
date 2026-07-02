#pragma once
#include "api_core.h"
#include "../py_uart.h"
#include "../py_scheduler.h"

extern PyUart py_uart;
extern PyScheduler py_scheduler;

inline void registerConsoleAPI() {

    // Enqueue a console command. Scheduled poll commands keep priority because
    // they are continuously re-enqueued; the console command runs when the
    // realtimeTask reaches it. Returns the current sequence so the frontend
    // knows which response is "old" and can wait for a newer one.
    apiGet("/req", []() {
        py_scheduler.enqueue(server.arg("code"));
        apiText(String(py_uart.getConsoleSeq()));
    });

    // Return a snapshot of the last captured console response as JSON. The
    // frontend polls this until cmd matches and seq advances, so a command
    // that needs several seconds (or waits behind priority polls) is never
    // cut off and no lines are lost.
    apiGet("/api/lastframe", []() {
        String raw, cmd; uint32_t seq;
        py_uart.getConsoleSnapshot(cmd, raw, seq);
        String txt;
        txt.reserve(raw.length() + 16);
        for (size_t i = 0; i < raw.length(); i++) {
            char c = raw[i];
            if (c == '\r') continue;          // CR overwrites lines in <textarea>
            else if (c == '\\') txt += "\\\\";
            else if (c == '"')  txt += "\\\"";
            else if (c == '\n') txt += "\\n";
            else if (c == '\t') txt += " ";    // tabs as space (keeps columns sane)
            else if ((uint8_t)c < 0x20) continue; // strip other control chars
            else txt += c;
        }
        String json = "{\"seq\":" + String(seq) +
                      ",\"cmd\":\"" + cmd +
                      "\",\"text\":\"" + txt + "\"}";
        apiJson(json);
    });
}
