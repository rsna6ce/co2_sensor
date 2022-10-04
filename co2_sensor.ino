#include "esp_system.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <TM1637Display.h> //TM1637 by Avishay Orpaz ver1.2.0
#include <CCS811.h>        //KS0457 keyestudio CCS811 Carbon Dioxide Air Quality Sensor

//TM1637 by Avishay Orpaz ver1.2.0
#define CLK 15
#define DIO 4
TM1637Display display(CLK, DIO);
uint8_t data[] = { 0xff, 0xff, 0xff, 0xff }; // all '1'

//KS0457 keyestudio CCS811 Carbon Dioxide Air Quality Sensor
CCS811 sensor;
int current_co2ppm = 0;
int current_tvocppb = 0;

// webserver
const char* ssid = "****";
const char* password = "****";
WebServer server(80);
String current_ipaddr = "";
#define INNER_LED 13


void handleRoot() {
  digitalWrite(INNER_LED, 1);
  String json = "{ ";
  json += "\"co2_ppm\":" + String(current_co2ppm) + ", ";
  json += "\"tvoc_ppb\":" + String(current_tvocppb) + " }";
  server.send(200, "text/plain", json);
  digitalWrite(INNER_LED, 0);
}

void handleMonitoring() {
    String resHtml = 
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "  <head>\n"
        "    <meta charset=\"utf-8\">\n"
        "    <title> monitoring</title>\n"
        "  </head>\n"
        "  <body>\n"
        "    <p id=\"co2_ppm\">co2_ppm</p>\n"
        "    <p id=\"tvoc_ppb\">tvoc_ppb</p>\n"
        "  </body>\n"
        "  <script language=\"javascript\" type=\"text/javascript\">\n"
        "  function update_loop() {\n"
        "    var request = new XMLHttpRequest();\n"
        "    request.open(\"GET\", \"http://" + current_ipaddr +
        "/\", true);\n"
        "    request.responseType = 'json';\n"
        "    request.onload = function () {\n"
        "        var data = this.response;\n"
        "        document.getElementById(\"co2_ppm\").textContent = \"co2 : \" + data.co2_ppm + \"[ppm]\";\n"
        "        document.getElementById(\"tvoc_ppb\").textContent = \"tvoc : \" + data.tvoc_ppb + \"[ppb]\";\n"
        "        setTimeout(update_loop, 1000);\n"
        "    };\n"
        "    request.onerror = function () {\n"
        "        document.getElementById(\"co2_ppm\").textContent = \"co2 : ??? [ppm]\";\n"
        "        document.getElementById(\"tvoc_ppb\").textContent = \"tvoc : ??? [ppb]\";\n"
        "        setTimeout(update_loop, 1000);\n"
        "    };\n"
        "    request.send();\n"
        "  };\n"
        "  window.onload = update_loop;\n"
        "  </script>\n"
        "</html>";
    server.send(200, "text/HTML", resHtml);
}

void handleNotFound() {
  digitalWrite(INNER_LED, 1);
  server.send(404, "text/plain", "404 page not found.");
  digitalWrite(INNER_LED, 0);
}

void showDisplayIpaddress(IPAddress& ipaddr) {
    for (int i=0; i<4; i++) {
        data[0] = display.encodeDigit((ipaddr[i] % 1000) / 100);
        data[1] = display.encodeDigit((ipaddr[i] %  100) /  10);
        data[2] = display.encodeDigit((ipaddr[i] %   10));
        data[3] = (i<3 ? 0x40 : 0x00);
        display.setSegments(data);
        delay(3000);
    }
}

void setup() {
    Serial.begin(115200);

    display.setBrightness(0x0f);
    while(sensor.begin() != 0){
        Serial.println("failed to init chip, please check if the chip connection is fine");
        delay(1000);
    }
    sensor.setMeasCycle(sensor.eCycle_250ms);

    pinMode(INNER_LED, OUTPUT);
    digitalWrite(INNER_LED, LOW);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    int blink_led = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        digitalWrite(INNER_LED, ((blink_led++)&1));
        data[0] = (blink_led&1) << 4;
        data[1] = (blink_led&1) << 4;
        data[2] = (blink_led&1) << 4;
        data[3] = (blink_led&1) << 4;
        display.setSegments(data);
        Serial.print(".");
    }
    digitalWrite(INNER_LED, LOW);
    Serial.println("");
    Serial.print("Connected to ");
    Serial.println(ssid);
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    IPAddress ipaddr = WiFi.localIP();
    current_ipaddr = ipaddr.toString();
    showDisplayIpaddress(ipaddr);

    if (MDNS.begin("esp32")) {
        Serial.println("MDNS responder started");
    }

    server.on("/", handleRoot);
    server.on("/monitoring",handleMonitoring);
    server.onNotFound(handleNotFound);
    server.begin();

    xTaskCreatePinnedToCore(loop2, "loop2", 4096, NULL, 1, NULL, 0);
    Serial.println("setup finished.");
}

void loop2(void * params) {
    while (true) {
    delay(1000);
        if(sensor.checkDataReady()){
            current_co2ppm = sensor.getCO2PPM();
            current_tvocppb = sensor.getTVOCPPB();
            Serial.println(current_co2ppm);
            data[0] = current_co2ppm >= 1000 ? display.encodeDigit((current_co2ppm / 1000)) : 0;
            data[1] = display.encodeDigit((current_co2ppm % 1000) / 100);
            data[2] = display.encodeDigit((current_co2ppm %  100) /  10);
            data[3] = display.encodeDigit((current_co2ppm %   10));
            display.setSegments(data);
        } else {
            current_co2ppm = -1;
            current_tvocppb = -1;
            data[0] = display.encodeDigit(0x0E);
            data[1] = display.encodeDigit(0x0E | (1<<7));
            data[2] = display.encodeDigit(0x0E);
            data[3] = display.encodeDigit(0x0E);
            display.setSegments(data);
        }
    }
}

void loop() {
    server.handleClient();
    delay(2);
}
