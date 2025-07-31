#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>

// --- WiFi config ---
const char *ssid = "IOT_2";
const char *password = "iot@1234";

// --- MQTT config ---
const char *mqtt_server = "mqttvcloud.innoway.vn";
const int mqtt_port = 1883;
const char *mqtt_user = "9e518af8-8569-4560-b089-d14247522e28";
const char *mqtt_pass = "aSytDmGSJAgVpUMxqOeQ05l8FD656yHO";
const char *mqtt_topic_sub = "messages/9e518af8-8569-4560-b089-d14247522e28/fota_request";

// --- WiFi + MQTT clients ---
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// --- WiFi connect ---
void setup_wifi()
{
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected. IP: " + WiFi.localIP().toString());
}

// --- MQTT reconnect ---
void reconnect()
{
  while (!mqttClient.connected())
  {
    Serial.print("Connecting to MQTT...");
    if (mqttClient.connect("esp32-client", mqtt_user, mqtt_pass))
    {
      Serial.println("connected");
      mqttClient.subscribe(mqtt_topic_sub);
      Serial.println("Subscribed to: " + String(mqtt_topic_sub));
    }
    else
    {
      Serial.print("Failed, rc=");
      Serial.print(mqttClient.state());
      delay(2000);
    }
  }
}

// --- Perform OTA from url_api ---
void performOTA(String url)
{
  Serial.println("Starting OTA from: " + url);
  WiFiClient client;
  HTTPClient http;
  http.begin(client, url);
  int httpCode = http.GET();
  if (httpCode == 200)
  {
    int len = http.getSize();
    WiFiClient *stream = http.getStreamPtr();
    if (!Update.begin(len))
    {
      Serial.println("Update.begin() failed");
      return;
    }
    size_t written = Update.writeStream(*stream);
    if (Update.end())
    {
      if (Update.isFinished())
      {
        Serial.println("OTA Update completed. Rebooting...");
        ESP.restart();
      }
      else
      {
        Serial.println("Update not finished.");
      }
    }
    else
    {
      Serial.printf("Update failed. Error #: %d\n", Update.getError());
    }
  }
  else
  {
    Serial.printf("HTTP GET failed, code: %d\n", httpCode);
  }
  http.end();
}

// --- MQTT callback using ArduinoJson ---
void callback(char *topic, byte *payload, unsigned int length)
{
  Serial.println("\n--- MQTT Message Received ---");
  Serial.println("Topic: " + String(topic));

  // Convert payload to String
  String msg;
  for (unsigned int i = 0; i < length; i++)
    msg += (char)payload[i];
  Serial.println("Payload: " + msg);

  // Parse JSON
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err)
  {
    Serial.print("JSON parse error: ");
    Serial.println(err.c_str());
    return;
  }

  String url_api = doc["url_api"] | "";
  String version = doc["version"] | "";
  String name = doc["name"] | "";
  String id = doc["id"] | "";

  if (url_api != "")
  {
    Serial.println("Parsed OTA Info:");
    Serial.println("  Firmware Name : " + name);
    Serial.println("  Version       : " + version);
    Serial.println("  URL           : " + url_api);
    performOTA(url_api);
  }
  else
  {
    Serial.println("Missing 'url_api' in payload.");
  }
}

void setup()
{
  Serial.begin(115200);
  setup_wifi();
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(callback);
}

void loop()
{
  if (!mqttClient.connected())
  {
    reconnect();
  }
  mqttClient.loop();
}