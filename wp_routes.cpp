#include "wp_routes.h"
#include "wp_webserver.h"

#include "web/api_core.h"
//#include "web/dashboard_api.h"
//#include "web/filemanager_api.h"
//#include "web/runtime_api.h"
#include "web/api_combined.h"
#include "web/console_api.h"
#include "web/wp_connect_api.h"
#include "web/pwr_api.h"
#include "web/bat_api.h"
#include "web/stat_api.h"
//#include "web/api_pylontech.h"
#include "web/wp_health_api.h"
#include "web/service_api.h"
#include "web/framedump_api.h"
//#include "web/cpu_api.h"

void registerRoutes() {

    registerServiceAPI(server);
    //registerDashboardAPI();
    //registerFileManagerAPI();
    //registerRuntimeAPI();
    registerCombinedAPI();
    registerConsoleAPI();
    registerPwrAPI();
    registerBatAPI();
    registerStatAPI();
    //registerPylontechAPI();
    registerHealthAPI();
    registerFramedumpApi();

    server.on("/api/wifi", HTTP_GET, apiWifiGet);
    server.on("/api/wifi", HTTP_POST, apiWifiPost);
    server.on("/api/wifi/scan", HTTP_GET, apiWifiScan);

    server.on("/api/mqtt", HTTP_GET, apiMqttGet);
    server.on("/api/mqtt", HTTP_POST, apiMqttPost);

    server.on("/api/time", HTTP_GET, apiTimeGet);
    server.on("/api/time", HTTP_POST, apiTimePost);

    server.on("/api/network", HTTP_GET, apiNetworkGet);
    server.on("/api/network", HTTP_POST, apiNetworkPost);

    server.serveStatic("/", SPIFFS, "/index.html");
    server.serveStatic("/", SPIFFS, "/");

    //server.on("/api/cpu", HTTP_GET, handleCpuApi);
}
