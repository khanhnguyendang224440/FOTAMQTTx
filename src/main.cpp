#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>

// ================== Firmware label (đổi khi ra bản mới) ==================
const char *FW_LABEL = "khanhdeptrai1";

// ================== WiFi ==================
const char *WIFI_SSID = "IOT_2";
const char *WIFI_PASS = "iot@1234";

// ================== MQTT (VTS Cloud) ==================
const char *MQTT_HOST = "mqttvcloud.innoway.vn";
const uint16_t MQTT_PORT = 1883;

// Username = DeviceID, Password = Token
const char *DEVICE_ID = "ed360af2-be80-46a3-ba5a-19d7a8d34b69";
const char *DEVICE_TOKEN = "Uo9H7he0H6S6r1NlbvIhp23FNca2Xrsl";

// Topic cloud phản hồi FOTA (theo hệ thống của bạn)
String TOPIC_FOTA_RESP = String("messages/") + DEVICE_ID + "/fota_request";

WiFiClient netClient;       // cho HTTP (http) & MQTT
WiFiClientSecure tlsClient; // cho HTTP (https)
PubSubClient mqtt(netClient);

unsigned long lastPrint = 0;

void connectWiFi()
{
  Serial.print("Connecting WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print('.');
    delay(500);
  }
  Serial.print("\nWiFi IP: ");
  Serial.println(WiFi.localIP());
}

bool connectMQTT()
{
  if (mqtt.connected())
    return true;
  String clientId = "esp32-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("Connecting MQTT...");
  bool ok = mqtt.connect(clientId.c_str(), DEVICE_ID, DEVICE_TOKEN);
  if (!ok)
  {
    Serial.print(" failed, rc=");
    Serial.println(mqtt.state());
    return false;
  }
  Serial.println(" connected");
  mqtt.subscribe(TOPIC_FOTA_RESP.c_str());
  mqtt.publish((String("messages/") + DEVICE_ID + "/alive").c_str(), FW_LABEL);
  Serial.print("Subscribed: ");
  Serial.println(TOPIC_FOTA_RESP);
  return true;
}

bool doOTA_POST_OR_GET(const String &url, const String &method, const String &md5)
{
  Serial.println("OTA URL: " + url + " | method=" + (method.length() ? method : "(POST default)"));

  HTTPClient http;
  bool isHTTPS = url.startsWith("https://");
  if (isHTTPS)
  {
    tlsClient.setInsecure();
    http.begin(tlsClient, url);
  }
  else
  {
    http.begin(netClient, url);
  }

  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int httpCode = method.equalsIgnoreCase("GET") ? http.GET()
                                                : http.POST((uint8_t *)nullptr, 0); // POST rỗng
  if (httpCode != HTTP_CODE_OK)
  {
    Serial.printf("HTTP %s failed, code: %d\n", method.length() ? method.c_str() : "POST", httpCode);
    http.end();
    return false;
  }

  int len = http.getSize(); // có thể -1 (chunked)
  WiFiClient *stream = http.getStreamPtr();

  bool ok = (len > 0) ? Update.begin(len) : Update.begin(UPDATE_SIZE_UNKNOWN);
  if (!ok)
  {
    Serial.printf("Update.begin failed: %d\n", Update.getError());
    http.end();
    return false;
  }

  if (md5.length() == 32)
  {
    Serial.println("Set MD5");
    Update.setMD5(md5.c_str());
  }

  size_t written = Update.writeStream(*stream);
  Serial.printf("OTA written: %u bytes\n", (unsigned)written);

  if (!Update.end())
  {
    Serial.printf("Update.end failed: %d\n", Update.getError());
    http.end();
    return false;
  }
  if (!Update.isFinished())
  {
    Serial.println("Update not finished.");
    http.end();
    return false;
  }

  http.end();
  Serial.println("OTA done. Rebooting...");
  Serial.flush();
  delay(200);
  ESP.restart();
  return true;
}

void onMessage(char *topic, byte *payload, unsigned int length)
{
  Serial.println("\n--- MQTT IN ---");
  Serial.print("Topic: ");
  Serial.println(topic);

  String msg;
  msg.reserve(length + 1);
  for (unsigned i = 0; i < length; i++)
    msg += (char)payload[i];
  Serial.println("Payload: " + msg);

  StaticJsonDocument<1024> doc;
  auto err = deserializeJson(doc, msg);
  if (err)
  {
    Serial.print("JSON error: ");
    Serial.println(err.c_str());
    return;
  }

  String url = doc["url_api"] | "";
  String method = doc["method"] | "";
  String checksum = doc["check_sum"] | "";

  if (!url.length())
  {
    Serial.println("Missing url_api");
    return;
  }
  if (!doOTA_POST_OR_GET(url, method, checksum))
    Serial.println("OTA failed.");
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("==== BOOT ====");
  Serial.print("FW_LABEL: ");
  Serial.println(FW_LABEL); // <- sẽ in "khanhdeptrai1" ở đây

  connectWiFi();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(2048); // JSON dài

  for (int i = 0; i < 5 && !connectMQTT(); i++)
    delay(1000);
  Serial.println("Ready. Publish rồi disconnect để tránh đá kết nối.");
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED)
    connectWiFi();
  if (!mqtt.connected())
    connectMQTT();
  mqtt.loop();

  // In nhãn firmware định kỳ để bạn dễ thấy trên monitor
  if (millis() - lastPrint > 10000)
  {
    lastPrint = millis();
    Serial.print("Running FW: ");
    Serial.println(FW_LABEL);
  }
}
