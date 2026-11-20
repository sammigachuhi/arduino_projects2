#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LiquidCrystal.h>
#include "secrets.h"

// const char* ssid = "xxx";
// const char* password = "xxx";
// const char* apiKey = "xxx";
// Matches your 4-bit LCD wiring: RS=19, E=23, D4=18, D5=17, D6=16, D7=15

LiquidCrystal lcd(19, 23, 18, 17, 16, 15);

String activeReply = "Chat Ready!";

bool shouldScroll = false;

int scrollPos = 0;
unsigned long lastStep = 0;

void showLines(const String& l1, const String& l2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(l1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(l2.substring(0, 16));
}

String cleanForLCD(String text) {
  text.replace("\r", " ");
  text.replace("\n", " ");
  text.replace("*", "");
  text.replace("\"", "'");
  text.trim();
  for (size_t i = 0; i < text.length(); i++) {
    uint8_t c = (uint8_t)text[i];
    if (c < 32 || c > 126) {
      text.setCharAt(i, ' ');
    }
  }
  return text;
}

String askGemini(const String& question) {
  if (WiFi.status() != WL_CONNECTED) {
    return "WiFi disconnected";
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  const char* modelName = "gemini-3.5-flash-lite";
  String url = String("https://generativelanguage.googleapis.com/v1beta/models/") + modelName + ":generateContent?key=" + apiKey;
  if (!http.begin(client, url)) {
    return "HTTP begin failed";
  }
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);
  JsonDocument req;
  JsonObject sysInst = req["system_instruction"];
  JsonArray sysParts = sysInst["parts"].to<JsonArray>();
  sysParts.add<JsonObject>()["text"] =
    "You are a warm, witty, human-like conversationalist. "
    "Answer in 1 to 2 short, natural sentences using plain ASCII only. No emojis or markdown.";
  JsonArray contents = req["contents"].to<JsonArray>();
  JsonObject userMsg = contents.add<JsonObject>();
  userMsg["role"] = "user";
  userMsg["parts"].to<JsonArray>().add<JsonObject>()["text"] = question;
  JsonObject genCfg = req["generationConfig"];
  genCfg["maxOutputTokens"] = 90;
  genCfg["temperature"] = 0.8;
  String body;
  serializeJson(req, body);
  int httpCode = http.POST(body);
  if (httpCode <= 0) {
    String err = http.errorToString(httpCode);
    http.end();
    return "Net err: " + err;
  }
  String responsePayload = http.getString();
  http.end();
  if (httpCode != 200) {
    return "API HTTP " + String(httpCode);
  }
  JsonDocument res;
  DeserializationError err = deserializeJson(res, responsePayload);
  if (err) {
    return "JSON parse error";
  }
  const char* reply = res["candidates"][0]["content"]["parts"][0]["text"];
  if (!reply) {
    return "No text returned";
  }
  return cleanForLCD(String(reply));
}

void startReplyDisplay(const String& reply) {
  activeReply = reply;
  scrollPos = 0;
  showLines(activeReply, "");
  if (activeReply.length() > 16) {
    shouldScroll = true;
    lastStep = millis() + 900;  // Pause briefly at the start before scrolling
  } else {
    shouldScroll = false;
  }
}
void updateScrollingLCD() {
  if (!shouldScroll) {
    return;
  }
  const unsigned long stepIntervalMs = 420;  // Increase for even slower scrolling (e.g. 500-600)
  if (millis() - lastStep < stepIntervalMs) {
    return;
  }
  lastStep = millis();
  scrollPos++;
  // Pad trailing spaces so the end of the sentence can clear across the 16-char row
  String padded = activeReply + "                ";
  int maxStart = (int)activeReply.length() - 1;
  if (scrollPos > maxStart) {
    shouldScroll = false;
    return;
  }
  lcd.setCursor(0, 0);
  lcd.print(padded.substring(scrollPos, scrollPos + 16));
}

void setup() {
  Serial.begin(115200);
  lcd.begin(16, 2);
  showLines("Connecting WiFi", "");
  WiFi.begin(ssid, password);
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 20000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected! Type a message and press Enter.");
    startReplyDisplay("Chat Ready!");
  } else {
    Serial.println("\nWiFi failed. Check SSID/password.");
    startReplyDisplay("WiFi Failed");
  }
}

void loop() {
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.length() == 0) return;
    shouldScroll = false;
    Serial.println("You: " + input);
    showLines("Thinking...", "");
    String answer = askGemini(input);
    Serial.println("ESP32: " + answer);
    startReplyDisplay(answer);
  }
  updateScrollingLCD();
}
