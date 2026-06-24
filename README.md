# Pylontech Battery Monitoring via WiFi and LAN
Forked from irekzielinski/Pylontech-Battery-Monitoring

This project allows you to control and monitor Pylontech US2000B, US2000C, US3000C and US5000 batteries via console port over WiFi.
It it's a great starting point to integrate battery with your home automation.

**I ACCEPT NO RESPONSIBILTY FOR ANY DAMAGE CAUSED, PROCEED AT YOUR OWN RISK**

# Features:
  * Low cost (around 20$ in total).
  * Adds WiFi capability to your Pylontech US2000B/C , US3000C, US5000 battery.
  * Device exposes web interface that allows to:
    * send console commands and read response over WiFi (no PC needed)
  * MQTT support:
    * device pushes basic battery data like SOC, temperature, state, etc to selected MQTT server
  * Easy to modify code using Arduino IDE and flash new firmware over WiFi (no need to disconnect from the battery).
  * choose dhcp or static ip

See the project in action on [Youtube](https://youtu.be/7VyQjKU3MsU):</br>
<a href="http://www.youtube.com/watch?feature=player_embedded&v=7VyQjKU3MsU" target="_blank"><img src="http://img.youtube.com/vi/7VyQjKU3MsU/0.jpg" alt="See the project in action on YouTube" width="240" height="180" border="10" /></a>


# Parts needed and schematics:
  * [ESP32 microcontroller](https://www.amazon.de/AZDelivery-ESP32-NodeMCU-gratis-eBook/dp/B07Z83MF5W/ref=pd_ci_mcx_mh_mcx_views_0_title?pd_rd_w=F1gm6&content-id=amzn1.sym.bbac26bb-3f7b-44dd-a8a5-c10fcfb1ed60%3Aamzn1.symc.30e3dbb4-8dd8-4bad-b7a1-a45bcdbc49b8&pf_rd_p=bbac26bb-3f7b-44dd-a8a5-c10fcfb1ed60&pf_rd_r=NSVTMZP7XDPWSGFW0VPQ&pd_rd_wg=rDzwP&pd_rd_r=23b42ddb-2882-4bb7-99d5-8fc9b6ff34ed&pd_rd_i=B07Z83MF5W&th=1).
  * [MAX3232 Transceiver](https://www.amazon.de/Adafruit-UART-zu-RS-232-Pegelwandler-MAX3232E-5987/dp/B0DXVYQ6CZ/ref=sr_1_3?__mk_de_DE=%C3%85M%C3%85%C5%BD%C3%95%C3%91&crid=M95NGDYAWDCD&dib=eyJ2IjoiMSJ9.ImSP7U9MuW_Mzo3jNmdsGGRtMNQV8sU2QM7wwLa9vySFDyNBPMxljJRjLHLVYE4PZGGg_k0H8RQ5XC5Ue-czZSCvmqiH65OlVfAKBHx-8dYk95hSPfOhNuv9t60hk3Oi32puWgIGiuTbibebR2P_V1d5rntkv5AaK_4lA5LxFk4.jbAGmd8fL-RcicNDuPq8tzmL_gcDsRB1BI2lfwbVZ6s&dib_tag=se&keywords=max+3232&qid=1777919116&s=industrial&sprefix=max3232%2Cindustrial%2C114&sr=1-3).
  * US2000B: Cable with RJ11 connector (some RJ11 cables have only two wires, make sure to buy one that has all four wires present).
  * US2000C or US5000: Cable with RJ45 connector (see below for more details).
  * Capacitors C1: 10uF, C2: 0.1uF (this is not strictly required, but recommended as Wemos D1 can have large current spikes).

![Schematics](Schemetics.png)

# US2000C/US3000C/US5000 notes:
This battery uses RJ45 cable instead of RJ10. Schematics is the same only plug differs:
  * RJ45 Pin 3 (white-green) = R1IN
  * RJ45 Pin 6 (green)       = T1OUT
  * RJ45 Pin 8 (brown)       = GND
![image](https://user-images.githubusercontent.com/19826327/146428324-29e3f9bf-6cc3-415c-9d60-fa5ee3d65613.png)


# How to get going:
  * Get ESP32
  * Install arduino IDE and ESP32 libraries as
  * Open [PylontechMonitoring.ino](PylontechMonitoring.ino) in arduino IDE
  * Make sure to copy content of [libraries subdirectory](libraries) to [libraries of your Arduino IDE](https://forum.arduino.cc/index.php?topic=88380.0).
  * Connect ESP32 to the MAX3232 transreceiver
  * Connect transreceiver to RJ10/RJ45 as descibed in the schematics (all three lines need to be connected)
  * Connect RJ10/RJ45 to the serial port of the Pylontech US2000 battery. If you have multiple batteries - connect to the master one.
  * Connect ESP32 to the power via USB
  * Connect to WIFI pylontech-xxxx
  * browse "192.168.4.1/filemanager
  * upload all files from folder /data to the ESP (Website) 
  * broswe "http://192.168.4.1"
  * klick connection scan for WIFI and connect to your WIFI 
  * 30s after connecting to your WIFI the AP turns of (After refresh you can see the IP-Adress of the ESP) 
  * have fun to Discover my Projekt 
  * latest 60s after start all site are avaiable 
  
  * For use MQTT connect to your server 
  * "MQTT" = this value will be publish "Send" = Autodiscoverer for Homeassistant 
  
  * Battery Console: send commands directly to the Battery 
  

# Pylontech Battery Monitor

This project uses an **ESP8266** to read data from a Pylontech battery (via Serial) and publish it to an MQTT broker.  
It also supports an internal web interface and (optionally) OTA updates.

---

## Configuration Parameters

go to the ESP Website

Press boot button: 
  * 1x short enable WIFI-AP
  * 5x short Reset WIFI 
  * 15s long "factory reset" 
