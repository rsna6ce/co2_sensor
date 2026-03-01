#include "esp_system.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <TM1637Display.h> // TM1637 by Avishay Orpaz ver1.2.0
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "SPIFFSIni.h"

// Sensirion SCD4x library
#include <Wire.h>
#include <SensirionI2cScd4x.h>

// TM1637
#define CLK 15
#define DIO 4
TM1637Display display(CLK, DIO);
uint8_t data[] = { 0xff, 0xff, 0xff, 0xff };

// SCD4x sensor
SensirionI2cScd4x scd4x;

float calibration_bias = 0.0;
float calibration_gain = 1.0;

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
  esp_restart();
}

void handleCalibration() {
  digitalWrite(INNER_LED, 1);
  SPIFFSIni config("/config.ini", true);

  // check exist and value
  if (server.hasArg("gain") && server.arg("gain").length() > 0) {
    String val = server.arg("gain");
    if (isDigit(val[0]) || val[0] == '-' || val[0] == '.') {
      calibration_gain = val.toFloat();
      config.write("calibration_gain", val);
    }
  }

  if (server.hasArg("bias") && server.arg("bias").length() > 0) {
    String val = server.arg("bias");
    if (isDigit(val[0]) || val[0] == '-' || val[0] == '.') {
      calibration_bias = val.toFloat();
      config.write("calibration_bias", val);
    }
  }

  String json = "{ ";
  json += "\"status\":\"OK\", ";
  json += "\"calibration_gain\":" + String(calibration_gain, 3) + ", ";
  json += "\"calibration_bias\":" + String(calibration_bias, 3);
  json += " }";

  server.send(200, "application/json", json);
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
  if (config.exist("calibration_bias")) calibration_bias = config.read("calibration_bias").toFloat();
  if (config.exist("calibration_gain")) calibration_gain = config.read("calibration_gain").toFloat();

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

  // SCD4x initialization (2 arguments: Wire + address)
  scd4x.begin(Wire, 0x62);  // 0x62 is the standard I2C address for SCD40/41
  Serial.println("SCD4x begin called.");

  // Reset periodic measurement
  uint16_t error = scd4x.stopPeriodicMeasurement();
  if (error) {
    Serial.print("stopPeriodicMeasurement failed: ");
    Serial.println(error);
  } else {
    Serial.println("Stopped any previous measurement.");
  }
  delay(1200);  // Wait a bit

  bool factory_reset = false;
  Serial.println("To factory reset the scd4x, press the 'y' key within 3 seconds.");
  delay(3000);
  if (Serial.available() > 0) {
    int inbyte = Serial.read();
    if (inbyte == 'y') {
      factory_reset = true;
    }
    String flush_str = Serial.readString(); // Flush remaining input
  }
  if (factory_reset) {
    // Factory reset + FRC (for manual execution)
    // calibration start
    uint16_t correction_;
    uint16_t FRC = 400; // FRC target value
    Serial.println("calibration start ... please wait 3 minutes.");
    scd4x.stopPeriodicMeasurement(); // Stop periodic measurement mode
    delay(500);
    scd4x.performFactoryReset();      // Reset settings to factory defaults
    scd4x.startPeriodicMeasurement(); // Start periodic measurement mode
    delay(3 * 60 * 1000);             // Run normally for 3 minutes
    scd4x.stopPeriodicMeasurement();  // Stop periodic measurement mode
    delay(500);
    scd4x.performForcedRecalibration(FRC, correction_); // Execute FRC
    delay(1000);                                        // Wait 1 second after FRC

    // Restart normal measurement mode
    while (scd4x.startPeriodicMeasurement() == false) {}
    Serial.println("Completed."); // FRC completion message

    Serial.printf("FRC. %d\n", correction_); // Display FRC correction value
  }

  if (false) {
    // Atmospheric calibration (for manual execution)
    uint16_t target_co2 = 400;
    uint16_t correction = 0;
    uint16_t frc_error = scd4x.performForcedRecalibration(target_co2, correction);
    if (frc_error == 0) {
      Serial.print("FRC command OK. Correction: ");
      Serial.println(correction);
      if (correction == 65535) {
        Serial.println("FRC FAILED (0xFFFF) - check stable CO2 / wiring / delay");
      } else {
        Serial.println("FRC SUCCESS! (correction != 65535)");
      }
    } else {
      Serial.print("FRC error code: ");
      Serial.println(frc_error);
    }
    delay(1200);  // Wait a bit
  }

  uint16_t asc_error = scd4x.setAutomaticSelfCalibrationEnabled(false);
  if (asc_error == 0) {
    Serial.println("ASC enabled (auto calibration)");
  }

  // Start periodic measurement
  error = scd4x.startPeriodicMeasurement();
  if (error) {
    Serial.print("startPeriodicMeasurement failed: ");
    Serial.println(error);
  } else {
    Serial.println("SCD40/41 periodic measurement started (5 sec interval)");
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
    server.on("/calibration", handleCalibration);
    server.onNotFound(handleNotFound);
    server.begin();
  }
  xTaskCreatePinnedToCore(loop2, "loop2", 4096, NULL, 1, NULL, 0);
  Serial.println("setup finished.");
}

void loop2(void * params) {
  while (true) {
    delay(5500);  // 5 sec cycle + margin

    uint16_t co2 = 0;
    float temperature = 0.0f;
    float humidity = 0.0f;

    uint16_t error = scd4x.readMeasurement(co2, temperature, humidity);

    if (error) {
      Serial.print("readMeasurement error: ");
      Serial.println(error);
      current_co2ppm = -1;
      current_temperature = 0.0f;
      current_humidity = 0.0f;
      // Error display
      data[0] = display.encodeDigit(0x0E);
      data[1] = display.encodeDigit(0x0E | (1 << 7));
      data[2] = display.encodeDigit(0x0E);
      data[3] = display.encodeDigit(0x0E);
      display.setSegments(data);
    } else if (co2 == 0) {
      // Invalid value right after measurement start
      current_co2ppm = -1;
      current_temperature = 0.0f;
      current_humidity = 0.0f;
      // Waiting display
      data[0] = 0;
      data[1] = 0;
      data[2] = 0;
      data[3] = 0;
      display.setSegments(data);
    } else {
      // external parameter correction, gain and bias.
      float calibrated_co2 = ((float)co2 * calibration_gain) + calibration_bias;
      // Lower limit correction 400ppm
      if (calibrated_co2 < 400.0) {
          calibrated_co2 = 400.0 - (400.0 - calibrated_co2) / 3.0;
      }
      co2 = (uint16_t)(calibrated_co2 + 0.5);
      current_co2ppm = co2;
      current_temperature = temperature;
      current_humidity = humidity;

      Serial.print("CO2: "); Serial.print(co2);
      Serial.print(" ppm  Temp: "); Serial.print(temperature, 1);
      Serial.print(" C  Hum: "); Serial.print(humidity, 1);
      Serial.println(" %");

      // TM1637 display
      data[0] = current_co2ppm >= 1000 ? display.encodeDigit((current_co2ppm / 1000)) : 0;
      data[1] = display.encodeDigit((current_co2ppm % 1000) / 100);
      data[2] = display.encodeDigit((current_co2ppm % 100) / 10);
      data[3] = display.encodeDigit((current_co2ppm % 10));
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