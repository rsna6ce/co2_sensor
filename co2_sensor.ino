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

// ==================== AI換気扇用追加 ====================
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <time.h>
// =======================================================

// TM1637
#define CLK 15
#define DIO 4
TM1637Display display(CLK, DIO);
uint8_t data[] = { 0xff, 0xff, 0xff, 0xff };

// SCD4x sensor
SensirionI2cScd4x scd4x;

float calibration_bias = 0.0;
float calibration_gain = 1.0;

// ファン制御関連（追加）
const int FAN_CONTROL_PIN1 = 18;           // fan pwm | led indicator
const int FAN_CONTROL_PIN2 = 19;           // fan pwm | led indicator
int fancontrol_on  = 1500;                // ppm以上でファンON（LOW）
int fancontrol_off = 1200;                // ppm以下でファンOFF（HIGH）
int current_fancontrol = 0;               // 現在送信用（0 or 100）

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

const int PIN_SW=0; //onboard switch
bool buttonPressed = false;

// ==================== AI換気扇 追加 ====================
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ntp.nict.jp", 9 * 3600, 60000);

String gemini_key    = "";
String gas_id       = "";
String secret_token = "";
String mail_to      = "";

String lastSentDate = "";
TaskHandle_t aiTaskHandle = NULL;

// 新規追加：ファン稼働率積分
long fanIntegral = 0;      // 累積値（0 or 100）
int  fanSampleCount = 0;   // サンプル回数
// =======================================================

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

// ファン制御設定用ハンドラ（新規追加）
void handleFancontrol() {
  digitalWrite(INNER_LED, 1);
  SPIFFSIni config("/config.ini", true);

  bool changed = false;

  if (server.hasArg("on") && server.arg("on").length() > 0) {
    String val = server.arg("on");
    if (isDigit(val[0])) {
      fancontrol_on = val.toInt();
      config.write("fancontrol_on", String(fancontrol_on));
      changed = true;
    }
  }

  if (server.hasArg("off") && server.arg("off").length() > 0) {
    String val = server.arg("off");
    if (isDigit(val[0])) {
      fancontrol_off = val.toInt();
      config.write("fancontrol_off", String(fancontrol_off));
      changed = true;
    }
  }

  String json = "{ ";
  json += "\"status\":\"ok\", ";
  json += "\"fancontrol_on\":" + String(fancontrol_on) + ", ";
  json += "\"fancontrol_off\":" + String(fancontrol_off);
  json += " }";

  server.send(200, "application/json", json);
  digitalWrite(INNER_LED, 0);
}

// ==================== 新規追加：AIパラメータ設定ハンドラ ====================
void handleAiParam() {
  digitalWrite(INNER_LED, 1);
  SPIFFSIni config("/config.ini", true);

  bool changed = false;

  if (server.hasArg("gemini_key") && server.arg("gemini_key").length() > 0) {
    gemini_key = server.arg("gemini_key");
    config.write("gemini_key", gemini_key);
    changed = true;
  }
  if (server.hasArg("gas_id") && server.arg("gas_id").length() > 0) {
    gas_id = server.arg("gas_id");
    config.write("gas_id", gas_id);
    changed = true;
  }
  if (server.hasArg("secret_token") && server.arg("secret_token").length() > 0) {
    secret_token = server.arg("secret_token");
    config.write("secret_token", secret_token);
    changed = true;
  }
  if (server.hasArg("mail_to") && server.arg("mail_to").length() > 0) {
    mail_to = server.arg("mail_to");
    config.write("mail_to", mail_to);
    changed = true;
  }

  String json = "{ ";
  json += "\"status\":\"OK\", ";
  json += "\"gemini_key\":\"" + gemini_key + "\", ";
  json += "\"gas_id\":\"" + gas_id + "\", ";
  json += "\"secret_token\":\"" + secret_token + "\", ";
  json += "\"mail_to\":\"" + mail_to + "\"";
  json += " }";

  server.send(200, "application/json", json);
  digitalWrite(INNER_LED, 0);
}
// =======================================================

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

// ====================== AI換気扇 関数 ======================
void aiTask(void *pvParameters) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    Serial.println("=== [AI換気扇] AI問い合わせ開始 ===");
    
    // 推定作業時間計算
    float fanRate = (fanSampleCount > 0) ? (float)fanIntegral / (float)fanSampleCount / 100.0f : 0.0f;
    float measuredHours = (float)fanSampleCount / 60.0f;                    // 測定した時間（時間単位）
    float estimatedHours = fanRate * 1.1f * measuredHours;                // 補正後推定作業時間
    Serial.printf("[AI換気扇] ファン稼働率: %.1f%% → 推定作業時間: %.1f時間\n", fanRate * 100, estimatedHours);
    String workMessage = "ファン稼働率: " + String(fanRate * 100, 1) + "→ 推定作業時間: " + String(estimatedHours, 1) + "時間\n";

    String aiMessage = getGeminiMessage(estimatedHours);

    if (aiMessage.length() > 0) {
      Serial.println("取得メッセージ: " + aiMessage);
      Serial.println("=== [AI換気扇] メール送信開始 ===");
      sendEmailViaGAS(workMessage + aiMessage);
      // 時間と統計をリセット
      timeClient.update();
      time_t now = timeClient.getEpochTime();
      struct tm timeinfo;
      localtime_r(&now, &timeinfo);
      char dateStr[11];
      strftime(dateStr, sizeof(dateStr), "%Y/%m/%d", &timeinfo);
      String today = String(dateStr);

      lastSentDate = today;
      fanSampleCount = 0;
      fanIntegral = 0;
    } else {
      Serial.println("[AI換気扇] AIメッセージ取得失敗");
    }
  }
}

String getGeminiMessage(float estimatedHours) {
  if (gemini_key == "") {
    Serial.println("[Gemini] gemini_keyが設定されていません");
    return "";
  }

  String prompt = "今日の推定作業時間は約" + String(estimatedHours, 1) + "時間でした。";

  if (estimatedHours >= 8.0) {
    prompt += "今日は特にたくさん作業しました。";
  } else if (estimatedHours >= 4.0) {
    prompt += "今日はしっかり作業しました。";
  } else {
    prompt += "今日はゆったり過ごせました。";
  }

  prompt += "温かくて短い一言メッセージを日本語で50文字程度で一つ作って、メッセージだけを出力してください。";

  int timeout_ms = 45000;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(timeout_ms);

  HTTPClient http;
  String url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-flash-latest:generateContent?key=" + gemini_key;
  //String url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=" + gemini_key;
  
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(timeout_ms);

  String payload = "{"
    "\"contents\": [{\"parts\":[{\"text\":\"" + prompt + "\"}]}],"
    "\"generationConfig\": {\"maxOutputTokens\": 1024, \"temperature\": 0.85}"
  "}";
  Serial.println(payload);

  int httpCode = http.POST(payload);
  String result = "";

  if (httpCode == 200) {
    String response = http.getString();
    JsonDocument doc;
    if (deserializeJson(doc, response) == DeserializationError::Ok) {
      result = doc["candidates"][0]["content"]["parts"][0]["text"].as<String>();
    }
  } else {
    Serial.printf("[Gemini] HTTPエラー: %d\n", httpCode);
  }
  http.end();
  return result;
}

// https://github.com/rsna6ce/mail_sender_gas を使用してメール送信する
bool sendEmailViaGAS(const String& message) {
  if (gas_id == "" || secret_token == "" || mail_to == "") {
    Serial.println("[GAS] メール送信パラメータが不足しています");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  String gas_url = "https://script.google.com/macros/s/" + gas_id + "/exec";

  HTTPClient http;
  http.begin(client, gas_url);
  http.addHeader("Content-Type", "application/json");

  JsonDocument payloadDoc;
  payloadDoc["_token"] = secret_token;
  payloadDoc["_mailaddress"] = mail_to;
  payloadDoc["_mailtitle"] = "【AI換気扇】今日のメッセージ";
  payloadDoc["_mailmessage"] = message;

  String payloadStr;
  serializeJson(payloadDoc, payloadStr);

  int httpCode = http.POST(payloadStr);
  http.end();

  if (httpCode == 200 || httpCode == 301 || httpCode == 302) {
    Serial.println("[AI換気扇] メール送信成功！");
    return true;
  } else {
    Serial.printf("[AI換気扇] メール送信失敗: %d\n", httpCode);
    return false;
  }
}
// =======================================================

void setup() {
  Serial.begin(115200);

  // SPIFFS
  SPIFFSIni config("/config.ini", true);

  // ファン制御ピンの初期化（追加）
  pinMode(FAN_CONTROL_PIN1, OUTPUT);
  pinMode(FAN_CONTROL_PIN2, OUTPUT_OPEN_DRAIN);
  digitalWrite(FAN_CONTROL_PIN1, LOW);   // 初期：LOW（ファン OFF）
  digitalWrite(FAN_CONTROL_PIN2, HIGH);  // 初期：オープン（LED OFF）
  pinMode(PIN_SW, INPUT_PULLUP);

  String ssid = config.read("ssid");
  String pass = config.read("pass");
  current_gas_utl = config.read("gas_url");

  if (config.exist("calibration_bias")) calibration_bias = config.read("calibration_bias").toFloat();
  if (config.exist("calibration_gain")) calibration_gain = config.read("calibration_gain").toFloat();

  if (config.exist("fancontrol_on")) {
    fancontrol_on = config.read("fancontrol_on").toInt();
  }
  if (config.exist("fancontrol_off")) {
    fancontrol_off = config.read("fancontrol_off").toInt();
  }

  // ==================== AIパラメータ読み込み ====================
  if (config.exist("gemini_key")) gemini_key = config.read("gemini_key");
  if (config.exist("gas_id")) gas_id = config.read("gas_id");
  if (config.exist("secret_token")) secret_token = config.read("secret_token");
  if (config.exist("mail_to")) mail_to = config.read("mail_to");
  // =======================================================

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

  display.setBrightness(0x02);

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
    Serial.println("ASC disabled (auto calibration off)");
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
    server.on("/fancontrol", handleFancontrol);
    server.on("/aiparam", handleAiParam);
    server.onNotFound(handleNotFound);
    server.begin();
  }
  xTaskCreatePinnedToCore(loop2, "loop2", 4096, NULL, 1, NULL, 0);

  // ==================== AI換気扇 初期化 ====================
  timeClient.begin();
  timeClient.update();
  
  time_t now = timeClient.getEpochTime();
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char dateStr[11];
  strftime(dateStr, sizeof(dateStr), "%Y/%m/%d", &timeinfo);
  lastSentDate = String(dateStr);
  
  Serial.println("[AI換気扇] 最終送信日初期化: " + lastSentDate);

  xTaskCreatePinnedToCore(aiTask, "AI_Task", 8192, NULL, 1, &aiTaskHandle, 1);
  Serial.println("[AI換気扇] AI Task created.");
  // =======================================================

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

      // ファン制御ロジック（追加）
      if (current_co2ppm >= fancontrol_on) {
        digitalWrite(FAN_CONTROL_PIN1, HIGH);  // ファン ON
        digitalWrite(FAN_CONTROL_PIN2, LOW);   // LED ON
        current_fancontrol = 100;
      }
      else if (current_co2ppm <= fancontrol_off) {
        digitalWrite(FAN_CONTROL_PIN1, LOW);   // ファン OFF
        digitalWrite(FAN_CONTROL_PIN2, HIGH);  // LED OFF
        current_fancontrol = 0;
      }
      // 中間は前状態維持（ヒステリシス効果）
    }
  }
}

// https://github.com/rsna6ce/post_data_gas を使用してデータをアップロードする
void sendDataToGAS(float value1, float value2, float value3, int value4) {
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
    "\"headers\":[\"co2\",\"temperature\",\"humidity\",\"fan\"],"
    "\"datas\":[" 
      + String(value1, 0) + "," 
      + String(value2, 1) + "," 
      + String(value3, 1) + ","
      + String(value4) + 
    "],"
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
      sendDataToGAS(current_co2ppm, current_temperature, current_humidity, current_fancontrol);

      fanIntegral += current_fancontrol;
      fanSampleCount++;

      // ==================== AI換気扇 日付チェック ====================
      timeClient.update();
      time_t now = timeClient.getEpochTime();
      struct tm timeinfo;
      localtime_r(&now, &timeinfo);
      char dateStr[11];
      strftime(dateStr, sizeof(dateStr), "%Y/%m/%d", &timeinfo);
      String today = String(dateStr);

      if (today != lastSentDate) {
        Serial.println("[AI換気扇] 日付が変わりました → AIタスク起動");
        if (aiTaskHandle) {
          xTaskNotifyGive(aiTaskHandle);
        }
      }
      // ========================================================
    }
    // debug ========================================================
    if (digitalRead(PIN_SW) == LOW && !buttonPressed) {
      delay(50);
      if (digitalRead(PIN_SW) == LOW) {
        buttonPressed = true;
        if (aiTaskHandle) {
          xTaskNotifyGive(aiTaskHandle);
        }
      }
    }
    if (digitalRead(PIN_SW) == HIGH) {
      buttonPressed = false;
    }
    // ========================================================
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