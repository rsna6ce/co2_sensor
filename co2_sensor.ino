#include "esp_system.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <TM1637Display.h> // TM1637 by Avishay Orpaz ver1.2.0
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "SPIFFSIni.h"

// SparkFun SCD30 library (ZIP install from https://github.com/sparkfun/SparkFun_SCD30_Arduino_Library)
#include <Wire.h>
#include <SparkFun_SCD30_Arduino_Library.h>

// TM1637
#define CLK 15
#define DIO 4
TM1637Display display(CLK, DIO);
uint8_t data[] = { 0xff, 0xff, 0xff, 0xff };

// SCD30 sensor
SCD30 scd30;

// webserver
WebServer server(80);
String current_ipaddr = "";
int wifi_status = WL_DISCONNECTED;
#define WIFI_TIMEOUT 30
#define INNER_LED 13

// Measured values
int current_co2ppm = 0;
float current_temperature = 0.0f;
float current_humidity = 0.0f;
String current_gas_utl = "";

uint64_t latest_millis_send_gas = 0;
const uint64_t interval_send_gas = 60 * 1000;
const String project_name = "co2_monitor";

uint64_t latest_wifi_check = 30;
const uint64_t interval_wifi_check = 60 * 1000;
const uint64_t interval_wifi_reconnect = 30 * 1000;

void handleRoot() {
  digitalWrite(INNER_LED, 1);
  String json = "{ ";
  json += "\"co2_ppm\":" + String(current_co2ppm) + ", ";
  json += "\"temperature_c\":" + String(current_temperature, 1) + ", ";
  json += "\"humidity_pct\":" + String(current_humidity, 1);
  json += " }";
  server.send(200, "text/plain", json);
  digitalWrite(INNER_LED, 0);
}

void handleReboot() {
  digitalWrite(INNER_LED, 1);
  String json = "{ ";
  json += "\"reboot\":\"ok\"}";
  server.send(200, "text/plain", json);
  digitalWrite(INNER_LED, 0);
  SPIFFSIni config("/config.ini", true);
  config.write("recalibration", "1");
  esp_restart();
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
    "    <p id=\"temperature_c\">temperature</p>\n"
    "    <p id=\"humidity_pct\">humidity</p>\n"
    "  </body>\n"
    "  <script language=\"javascript\" type=\"text/javascript\">\n"
    "  function update_loop() {\n"
    "    var request = new XMLHttpRequest();\n"
    "    request.open(\"GET\", \"http://" + current_ipaddr +
    "/\", true);\n"
    "    request.responseType = 'json';\n"
    "    request.onload = function () {\n"
    "        var data = this.response;\n"
    "        document.getElementById(\"co2_ppm\").textContent = \"CO₂ : \" + data.co2_ppm + \" ppm\";\n"
    "        document.getElementById(\"temperature_c\").textContent = \"Temperature : \" + data.temperature_c.toFixed(1) + \" ℃\";\n"
    "        document.getElementById(\"humidity_pct\").textContent = \"Humidity : \" + data.humidity_pct.toFixed(1) + \" %\";\n"
    "        setTimeout(update_loop, 1000);\n"
    "    };\n"
    "    request.onerror = function () {\n"
    "        document.getElementById(\"co2_ppm\").textContent = \"CO₂ : ??? ppm\";\n"
    "        document.getElementById(\"temperature_c\").textContent = \"Temperature : ??? ℃\";\n"
    "        document.getElementById(\"humidity_pct\").textContent = \"Humidity : ??? %\";\n"
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
  for (int i = 0; i < 4; i++) {
    data[0] = display.encodeDigit((ipaddr[i] % 1000) / 100);
    data[1] = display.encodeDigit((ipaddr[i] % 100) / 10);
    data[2] = display.encodeDigit((ipaddr[i] % 10));
    data[3] = (i < 3 ? 0x40 : 0x00);
    display.setSegments(data);
    delay(3000);
  }
}

String serial_input_sync(String msg) {
  Serial.println(msg);
  while (Serial.available() == 0) {}
  Serial.setTimeout(60000);
  String input_str = Serial.readStringUntil('\n');
  Serial.setTimeout(1000);
  input_str.trim();
  return input_str;
}

void setup() {
  Serial.begin(115200);

  // SPIFFS
  SPIFFSIni config("/config.ini", true);
  String ssid = config.read("ssid");
  String pass = config.read("pass");
  current_gas_utl = config.read("gas_url");

  Serial.println("To reset the SSID, press the 'y' key within 3 seconds.");
  delay(3000);
  if (Serial.available() > 0) {
    int inbyte = Serial.read();
    if (inbyte == 'y') {
      ssid = "";
      pass = "";
      current_gas_utl = "";
    }
    String flush_str = Serial.readString(); // Flush remaining input
  }

  bool new_ssid_pass = false;
  if (ssid == "" || pass == "") {
    while (true) {
      Serial.println("Enter Wi-Fi settings: SSID and password.");
      ssid = serial_input_sync("SSID?");
      pass = serial_input_sync("Password?");
      current_gas_utl = serial_input_sync("GAS URL?");
      String confirm_msg = "SSID: " + ssid + "  Password: " + pass + "\r\n";
      String yes_no = serial_input_sync(confirm_msg + "OK? (yes/no)");
      if (yes_no == "yes" || yes_no == "y") {
        new_ssid_pass = true;
        config.write("ssid", ssid);
        config.write("pass", pass);
        config.write("gas_url", current_gas_utl);
        break;
      }
    }
  }

  display.setBrightness(0x05);

  Wire.begin();  // ESP32 default I2C (SDA=21, SCL=22)

  // SCD30 initialization
  if (scd30.begin() == false) {
    Serial.println("SCD30 not detected. Check wiring.");
  } else {
    Serial.println("SCD30 detected!");
  }

  // Optional: Set measurement interval (default is 2 seconds)
  scd30.setMeasurementInterval(5);  // 5 seconds (your loop2 delay is 5500ms)

  // Disable ASC (as per your current setting)
  scd30.setAutoSelfCalibration(true);
  Serial.println("ASC enabled");

  String recalibration = config.read("recalibration");
  if (recalibration == "1") {
    for (int i=0; i<300; i++) {
      int countdown = 300 - i;
      data[0] = display.encodeDigit(0);
      data[1] = display.encodeDigit((countdown % 1000) / 100);
      data[2] = display.encodeDigit((countdown % 100) / 10);
      data[3] = display.encodeDigit((countdown % 10));
      display.setSegments(data);
      delay(1000);
    }
    scd30.setForcedRecalibrationFactor(400);
    delay(3000);
    config.write("recalibration", "");
  }


  // Start periodic measurement
  if (scd30.beginMeasuring() == true) {
    Serial.println("SCD30 periodic measurement started (5 sec interval)");
  } else {
    Serial.println("Failed to start measurement");
  }

  pinMode(INNER_LED, OUTPUT);
  digitalWrite(INNER_LED, LOW);

  WiFi.mode(WIFI_STA);
  if (new_ssid_pass) {
    WiFi.begin(ssid, pass);
  } else {
    WiFi.begin();
  }
  int blink_led = 0;
  for (int i = 0; (i < WIFI_TIMEOUT * 2) && (wifi_status != WL_CONNECTED); i++) {
    wifi_status = WiFi.status();
    delay(500);
    digitalWrite(INNER_LED, ((blink_led++) & 1));
    data[0] = (blink_led & 1) << 6;
    data[1] = (blink_led & 1) << 6;
    data[2] = (blink_led & 1) << 6;
    data[3] = (blink_led & 1) << 6;
    display.setSegments(data);
    Serial.print(".");
  }
  digitalWrite(INNER_LED, LOW);
  if (wifi_status == WL_CONNECTED) {
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
    server.on("/monitoring", handleMonitoring);
    server.on("/reboot", handleReboot);
    server.onNotFound(handleNotFound);
    server.begin();
  }
  xTaskCreatePinnedToCore(loop2, "loop2", 4096, NULL, 1, NULL, 0);
  Serial.println("setup finished.");
}

void loop2(void * params) {
  while (true) {
    delay(5500);  // 5 sec cycle + margin

    if (scd30.dataAvailable()) {
      int temp_co2ppm = scd30.getCO2();
      if (temp_co2ppm < 400) {
          temp_co2ppm = (int)((400.0f - (400.0f - (float)temp_co2ppm) / 3.0f) + 0.5f);
      }
      current_co2ppm = temp_co2ppm;
      current_temperature = scd30.getTemperature();
      current_humidity = scd30.getHumidity();

      Serial.print("CO2: "); Serial.print(current_co2ppm);
      Serial.print(" ppm  Temp: "); Serial.print(current_temperature, 1);
      Serial.print(" C  Hum: "); Serial.print(current_humidity, 1);
      Serial.println(" %");

      // TM1637 display (CO2 only)
      data[0] = current_co2ppm >= 1000 ? display.encodeDigit((current_co2ppm / 1000)) : 0;
      data[1] = display.encodeDigit((current_co2ppm % 1000) / 100);
      data[2] = display.encodeDigit((current_co2ppm % 100) / 10);
      data[3] = display.encodeDigit((current_co2ppm % 10));
      display.setSegments(data);
    } else {
      Serial.println("No new SCD30 data available");
      current_co2ppm = -1;
      current_temperature = 0.0f;
      current_humidity = 0.0f;
      // Error display
      data[0] = display.encodeDigit(0x0E);
      data[1] = display.encodeDigit(0x0E | (1 << 7));
      data[2] = display.encodeDigit(0x0E);
      data[3] = display.encodeDigit(0x0E);
      display.setSegments(data);
    }
  }
}

void sendDataToGAS(float value1, float value2, float value3) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    return;
  }
  if (current_gas_utl == "") {
    // skip upload
    return;
  }

  Serial.print("Using GAS URL: ");
  Serial.println(current_gas_utl);

  WiFiClientSecure client;
  client.setInsecure();  // Skip certificate verification (for testing! Add root certs in production)

  HTTPClient http;

  if (!http.begin(client, current_gas_utl)) {
    Serial.println("http.begin() failed - URL or connection issue");
    return;
  }

  http.addHeader("Content-Type", "application/json");

  // JSON payload
  String jsonPayload = "{"
    "\"project_name\":\"" + project_name + "\","
    "\"headers\":[\"co2\",\"temperature\",\"humidity\"],"
    "\"datas\":[" + String(value1, 0) + "," + String(value2, 1) + "," + String(value3, 1) + "],"
    "\"max_count\":1440"
  "}";

  Serial.println("Sending JSON:");
  Serial.println(jsonPayload);

  int httpResponseCode = http.POST(jsonPayload);

  if (httpResponseCode > 0) {
    Serial.print("HTTP Code: "); Serial.println(httpResponseCode);
    if (httpResponseCode == 302 || httpResponseCode == 200) {
      Serial.println("GAS upload successful (including redirects)");
    } else {
      Serial.println("Unexpected response code");
    }
  } else {
    Serial.print("POST failed, error code: ");
    Serial.println(httpResponseCode);
    Serial.println(http.errorToString(httpResponseCode));  // Detailed error message
  }

  http.end();
}

void loop() {
  uint64_t current_millis = millis();
  if (wifi_status == WL_CONNECTED) {
    server.handleClient();
    if (latest_millis_send_gas + interval_send_gas < current_millis) {
      latest_millis_send_gas = current_millis;
      sendDataToGAS(current_co2ppm, current_temperature, current_humidity);
    }
  } else {
    if (latest_wifi_check + interval_wifi_check < current_millis) {
      latest_wifi_check = current_millis;
      Serial.println("WiFi disconnected. Attempting to reconnect...");
      WiFi.disconnect();
      WiFi.reconnect();

      unsigned long startAttemptTime = millis();
      while (WiFi.status() != WL_CONNECTED && 
             millis() - startAttemptTime < interval_wifi_reconnect) {
        delay(100);
        Serial.print(".");
      }
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi reconnected");
        Serial.println(WiFi.localIP());
      } else {
        Serial.println("\nFailed to reconnect to WiFi");
      }
    }
  }
  delay(2);
}