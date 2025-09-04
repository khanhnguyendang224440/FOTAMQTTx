#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <base64.h> // ESP32 core có sẵn hàm base64

const char *FW_LABEL = "khanhdeptrai1";
const char *WIFI_SSID = "IOT_2";
const char *WIFI_PASS = "iot@1234";
const char *MQTT_HOST = "mqttvht.innoway.vn";
const uint16_t MQTT_PORT = 1883;
const char *DEVICE_ID = "2ee0f815-d84f-4d89-ac5d-6d53dbe8d63d";
const char *DEVICE_TOKEN = "NHeFZn0Cmd8d4teCwtsdjoqVwPvGNrRQ";

String TOPIC_FOTA_LONG = String("messages/") + DEVICE_ID + "/fota_request";
const char *TOPIC_FOTA_SHORT = "fota_request";

WiFiClient mqttClient, httpClient;
WiFiClientSecure httpsClient;
PubSubClient mqtt(mqttClient);

void connectWiFi()
{
  if (WiFi.status() == WL_CONNECTED)
    return;
  Serial.print("Connecting WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print(".");
    delay(300);
  }
  Serial.println(" OK");
}

bool connectMQTT()
{
  if (mqtt.connected())
    return true;
  Serial.print("Connecting MQTT...");
  String cid = "esp32-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (!mqtt.connect(cid.c_str(), DEVICE_ID, DEVICE_TOKEN))
  {
    Serial.println(" fail");
    return false;
  }
  mqtt.subscribe(TOPIC_FOTA_SHORT);
  mqtt.subscribe(TOPIC_FOTA_LONG.c_str());
  mqtt.publish("alive", FW_LABEL, true);
  Serial.println(" OK");
  return true;
}

bool doUpdateFromHttp(HTTPClient &http, const String &md5)
{
  int len = http.getSize();
  Serial.printf("Content-Length: %d\n", len);
  if (!Update.begin(len > 0 ? len : UPDATE_SIZE_UNKNOWN))
    return false;
  if (md5.length() == 32)
  {
    Update.setMD5(md5.c_str());
    Serial.println("MD5 set");
  }
  WiFiClient *stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  Serial.printf("Written: %d bytes\n", written);
  bool ok = written > 0 && Update.end() && Update.isFinished();
  Serial.printf("Update result: %s\n", ok ? "OK" : "FAIL");
  return ok;
}

bool doOTA_GET(const String &url, const String &md5, const char *authScheme, const char *authCred)
{
  HTTPClient http;
  WiFiClient *client = url.startsWith("https://")
                           ? (httpsClient.setInsecure(), (WiFiClient *)&httpsClient)
                           : (WiFiClient *)&httpClient;

  if (!http.begin(*client, url))
    return false;
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.useHTTP10(true);
  http.addHeader("Connection", "close");
  http.addHeader("Accept", "application/octet-stream");
  if (authScheme && authCred)
    http.addHeader("Authorization", String(authScheme) + " " + authCred);

  int code = http.GET();
  Serial.printf("HTTP GET code: %d\n", code);
  if (code != HTTP_CODE_OK)
  {
    Serial.printf("Resp: %s\n", http.getString().c_str());
    http.end();
    return false;
  }
  bool ok = doUpdateFromHttp(http, md5);
  http.end();
  return ok;
}

bool doOTA_POST(const String &url, const String &md5, const String &id, const char *authScheme, const char *authCred)
{
  HTTPClient http;
  WiFiClient *client = url.startsWith("https://")
                           ? (httpsClient.setInsecure(), (WiFiClient *)&httpsClient)
                           : (WiFiClient *)&httpClient;

  if (!http.begin(*client, url))
    return false;
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.useHTTP10(true);
  http.addHeader("Connection", "close");
  http.addHeader("Accept", "application/octet-stream");
  http.addHeader("Content-Type", "application/json");
  if (authScheme && authCred)
    http.addHeader("Authorization", String(authScheme) + " " + authCred);

  String body = id.length() ? String("{\"id\":\"") + id + "\"}" : "{}";
  int code = http.POST(body);
  Serial.printf("HTTP POST code: %d\n", code);
  if (code != HTTP_CODE_OK)
  {
    Serial.printf("Resp: %s\n", http.getString().c_str());
    http.end();
    return false;
  }
  bool ok = doUpdateFromHttp(http, md5);
  http.end();
  return ok;
}

bool doOTA_fromMsg(const String &method, const String &url, const String &md5, const String &id)
{
  // thử Bearer trước
  if (method == "GET")
  {
    if (doOTA_GET(url, md5, "Bearer", DEVICE_TOKEN))
      return true;
    // fallback Basic
    String basicRaw = String(DEVICE_ID) + ":" + DEVICE_TOKEN;
    String basic = base64::encode(basicRaw);
    return doOTA_GET(url, md5, "Basic", basic.c_str());
  }
  else
  { // POST
    if (doOTA_POST(url, md5, id, "Bearer", DEVICE_TOKEN))
      return true;
    String basicRaw = String(DEVICE_ID) + ":" + DEVICE_TOKEN;
    String basic = base64::encode(basicRaw);
    return doOTA_POST(url, md5, id, "Basic", basic.c_str());
  }
}

void onMessage(char *topic, byte *payload, unsigned int length)
{
  if (!(String(topic) == TOPIC_FOTA_SHORT || String(topic) == TOPIC_FOTA_LONG))
    return;

  Serial.println("--- FOTA Request ---");
  String msg;
  msg.reserve(length + 1);
  for (unsigned i = 0; i < length; i++)
    msg += (char)payload[i];
  Serial.println(msg);

  StaticJsonDocument<768> d;
  if (deserializeJson(d, msg))
  {
    Serial.println("JSON parse fail");
    return;
  }

  String id = d["id"] | "";
  String url = d["url_api"] | "";
  String md5 = d["check_sum"] | "";
  String method = d["method"] | "GET";
  if (!url.length())
  {
    Serial.println("No URL, skip");
    return;
  }

  Serial.printf("OTA URL: %s (method=%s)\n", url.c_str(), method.c_str());
  if (doOTA_fromMsg(method, url, md5, id))
  {
    Serial.println("OTA done, rebooting...");
    delay(200);
    ESP.restart();
  }
  else
  {
    Serial.println("OTA failed");
  }
}

void setup()
{
  Serial.begin(115200);
  Serial.printf("\n==== BOOT ====\nFW_LABEL: %s\n", FW_LABEL);
  connectWiFi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(4096);
  mqtt.setCallback(onMessage);
  while (!connectMQTT())
    delay(800);
  Serial.println("Ready.");
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED)
    connectWiFi();
  if (!mqtt.connected())
    connectMQTT();
  else
    mqtt.loop();
}
