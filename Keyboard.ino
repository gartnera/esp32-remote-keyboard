#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiClient.h>
#include "RemoteDebug.h"
#include "secrets.h"

#include "Arduino.h"
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "esp32-hal-tinyusb.h"
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
USBHIDKeyboard Keyboard;

bool keyboardIsInit = false;

// TCP server at port 80 will respond to HTTP requests
WiFiServer httpServer(80);

WebSocketsServer webSocket = WebSocketsServer(81);

RemoteDebug Debug;

void maybeInitKeyboard()
{
  if (keyboardIsInit)
  {
    return;
  }
  Keyboard.begin();
  USB.begin();
  keyboardIsInit = true;
}

void processCmdRemoteDebug()
{
  String rawCmd = Debug.getLastCommand();
  int spaceIdx = rawCmd.indexOf(" ");
  String cmd = rawCmd;
  if (spaceIdx >= 0)
  {
    cmd = rawCmd.substring(0, spaceIdx);
  }

  if (cmd == "bench")
  {
    // Benchmark 1 - Printf
    debugA("* Benchmark 1 - one Printf");
    uint32_t timeBegin = millis();
    uint8_t times = 50;
    for (uint8_t i = 1; i <= times; i++)
    {
      debugA("%u - 1234567890 - AAAA", i);
    }
    debugA("* Time elapsed for %u printf: %ld ms.\n", times,
           (millis() - timeBegin));
  }
  else if (cmd == "bootloader")
  {
    usb_persist_restart(RESTART_BOOTLOADER);
  }
  else if (cmd == "keyboardinit")
  {
    maybeInitKeyboard();
  }
  else if (cmd == "keyboard")
  {
    char key = rawCmd[spaceIdx + 1];
    debugA("writing key: %c\n", key);
    Keyboard.write(key);
  }
  else if (cmd == "echoc")
  {
    char key = rawCmd[spaceIdx + 1];
    debugA("echo: %c\n", key);
  }
  else
  {
    debugA("unknown command %s\n", rawCmd);
  }
}

void setup(void)
{
  Serial.begin(115200);
  Serial.println("setup start");
  // Connect to WiFi network
  WiFi.begin(SECRET_WIFI_SSID, SECRET_WIFI_PASSWORD);
  Serial.println("wifi begin");

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Debug.begin("esp32");
  Debug.setResetCmdEnabled(true);
  Debug.showColors(true);
  Debug.setCallBackProjectCmds(&processCmdRemoteDebug);

  Serial.print("Connected to ");
  Serial.println(SECRET_WIFI_SSID);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // Set up mDNS responder:
  // - first argument is the domain name, in this example
  //   the fully-qualified domain name is "esp32.local"
  // - second argument is the IP address to advertise
  //   we send our IP address on the WiFi network
  if (!MDNS.begin("esp32"))
  {
    Serial.println("Error setting up MDNS responder!");
    while (1)
    {
      delay(1000);
    }
  }
  Debug.println("mDNS responder started");

  // Start TCP (HTTP) server
  httpServer.begin();
  Debug.println("TCP server started");

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  // Add service to MDNS-SD
  MDNS.addService("http", "tcp", 80);
}

void handleHttpClient()
{
  // Check if a client has connected
  WiFiClient client = httpServer.available();
  if (!client)
  {
    return;
  }
  Debug.println("");
  Debug.println("New client");

  // Wait for data from client to become available
  while (client.connected() && !client.available())
  {
    delay(1);
  }

  // Read the first line of HTTP request
  String req = client.readStringUntil('\r');

  // First line of HTTP request looks like "GET /path HTTP/1.1"
  // Retrieve the "/path" part by finding the spaces
  int addr_start = req.indexOf(' ');
  int addr_end = req.indexOf(' ', addr_start + 1);
  if (addr_start == -1 || addr_end == -1)
  {
    Debug.print("Invalid request: ");
    Debug.println(req);
    return;
  }
  req = req.substring(addr_start + 1, addr_end);
  Debug.print("Request: ");
  Debug.println(req);

  String s = "HTTP/1.1 200 OK\r\n";

  if (req == "/")
  {
    IPAddress ip = WiFi.localIP();
    String ipStr = String(ip[0]) + '.' + String(ip[1]) + '.' + String(ip[2]) + '.' + String(ip[3]);
    s += "Content-Type: text/html\r\n\r\n<!DOCTYPE HTML>\r\n<html>Hello from ESP32 at ";
    s += ipStr;
    s += "</html>\r\n\r\n";
    Debug.println("Sending 200");
  }
  else
  {
    s = "HTTP/1.1 404 Not Found\r\n\r\n";
    Debug.println("Sending 404");
  }
  client.print(s);

  client.stop();
  Debug.println("Done with client");
}

StaticJsonDocument<200> doc;

void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length)
{
  DeserializationError error;
  int keyCode;
  JsonArray modifiers;
  uint8_t modifier;

  switch (type)
  {
  case WStype_DISCONNECTED:
    Debug.printf("[%u] Disconnected!\n", num);
    break;
  case WStype_CONNECTED:
  {
    IPAddress ip = webSocket.remoteIP(num);
    Debug.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);

    maybeInitKeyboard();

    // send message to client
    webSocket.sendTXT(num, "Connected");
  }
  break;
  case WStype_TEXT:
    debugD("[%u] get Text: %s", num, payload);
    error = deserializeJson(doc, payload);

    // Test if parsing succeeds.
    if (error)
    {
      Debug.print(F("deserializeJson() failed: "));
      Debug.println(error.f_str());
      return;
    }
    keyCode = doc["keyCode"].as<uint8_t>();
    debugD("got keyCode: %d", keyCode);
    modifiers = doc["modifiers"].as<JsonArray>();
    for (JsonVariant v : modifiers)
    {
      modifier = v.as<uint8_t>();
      debugD("got modifier: %d", modifier);
      Keyboard.pressRaw(modifier);
    }
    Keyboard.pressRaw(keyCode);
    Keyboard.releaseAll();
    break;
  case WStype_ERROR:
  case WStype_FRAGMENT_TEXT_START:
  case WStype_FRAGMENT_BIN_START:
  case WStype_FRAGMENT:
  case WStype_FRAGMENT_FIN:
    Debug.printf("got other type: %d\n", type);
    break;
  }
}

void loop(void)
{
  handleHttpClient();
  Debug.handle();
  webSocket.loop();
  yield();
}
