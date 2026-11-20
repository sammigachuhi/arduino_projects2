#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LiquidCrystal.h>
#include "secrets.h"

// Matches your 4-bit LCD wiring: RS=19, E=23, D4=18, D5=17, D6=16, D7=15
LiquidCrystal lcd(19, 23, 18, 17, 16, 15);
WebServer server(80);

String activeReply = "Chat Ready!";
bool shouldScroll = false;
int scrollPos = 0;
unsigned long lastStep = 0;

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>ESP32 LCD Chat</title>
  <style>
    body { font-family: system-ui, sans-serif; max-width: 560px; margin: 2rem auto; padding: 0 1rem; background: #111; color: #f3f4f6; }
    h1 { font-size: 1.35rem; margin-bottom: 0.25rem; }
    p.sub { color: #9ca3af; font-size: 0.9rem; margin-top: 0; }
    form { display: flex; gap: 0.5rem; margin: 1rem 0; }
    input { flex: 1; padding: 0.7rem 0.85rem; border-radius: 8px; border: 1px solid #374151; background: #1f2937; color: #fff; font-size: 1rem; }
    button { padding: 0.7rem 1.1rem; border-radius: 8px; border: 0; background: #2563eb; color: #fff; font-weight: 600; cursor: pointer; }
    button:disabled { opacity: 0.5; cursor: not-allowed; }
    .box { background: #1f2937; border: 1px solid #374151; border-radius: 8px; padding: 1rem; min-height: 5rem; white-space: pre-wrap; }
    .meta { color: #9ca3af; font-size: 0.8rem; margin-top: 0.5rem; }
  </style>
</head>
<body>
  <h1>ESP32 LCD Question Console</h1>
  <p class="sub">Questions sent here are answered on the 16x2 LCD and shown below.</p>
  <form id="askForm">
    <input id="qInput" type="text" placeholder="Ask something..." autocomplete="off" required />
    <button id="sendBtn" type="submit">Send</button>
  </form>
  <div id="output" class="box">Ready.</div>
  <div id="status" class="meta">Connected to ESP32</div>

  <script>
    const form = document.getElementById('askForm');
    const input = document.getElementById('qInput');
    const btn = document.getElementById('sendBtn');
    const out = document.getElementById('output');
    const status = document.getElementById('status');

    form.addEventListener('submit', async (e) => {
      e.preventDefault();
      const question = input.value.trim();
      if (!question) return;

      btn.disabled = true;
      status.textContent = 'Sending to ESP32 & waiting for Gemini...';
      out.textContent = 'You: ' + question + '\nLCD: Thinking...';

      try {
        const res = await fetch('/ask', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ question })
        });
        const data = await res.json();
        out.textContent = 'You: ' + question + '\nLCD: ' + (data.reply || 'No reply');
        status.textContent = 'Displayed on LCD';
        input.value = '';
        input.focus();
      } catch (err) {
        status.textContent = 'Request failed: ' + err.message;
      } finally {
        btn.disabled = false;
      }
    });
  </script>
</body>
</html>
)rawliteral";

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
    lastStep = millis() + 900;
  } else {
    shouldScroll = false;
  }
}

void updateScrollingLCD() {
  if (!shouldScroll) {
    return;
  }
  const unsigned long stepIntervalMs = 420;
  if (millis() - lastStep < stepIntervalMs) {
    return;
  }
  lastStep = millis();
  scrollPos++;
  String padded = activeReply + "                ";
  int maxStart = (int)activeReply.length() - 1;
  if (scrollPos > maxStart) {
    shouldScroll = false;
    return;
  }
  lcd.setCursor(0, 0);
  lcd.print(padded.substring(scrollPos, scrollPos + 16));
}

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleAsk() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"Missing body\"}");
    return;
  }

  JsonDocument reqDoc;
  if (deserializeJson(reqDoc, server.arg("plain")) != DeserializationError::Ok) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  String question = reqDoc["question"] | "";
  question.trim();
  if (question.length() == 0) {
    server.send(400, "application/json", "{\"error\":\"Empty question\"}");
    return;
  }

  shouldScroll = false;
  Serial.println("Web: " + question);
  showLines("Thinking...", "");

  String answer = askGemini(question);
  Serial.println("ESP32: " + answer);
  startReplyDisplay(answer);

  JsonDocument respDoc;
  respDoc["reply"] = answer;
  String outJson;
  serializeJson(respDoc, outJson);
  server.send(200, "application/json", outJson);
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
    Serial.println("\nWiFi connected!");
    Serial.print("Open http://");
    Serial.println(WiFi.localIP());

    server.on("/", HTTP_GET, handleRoot);
    server.on("/ask", HTTP_POST, handleAsk);
    server.begin();

    startReplyDisplay("IP:" + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi failed. Check SSID/password.");
    startReplyDisplay("WiFi Failed");
  }
}

void loop() {
  server.handleClient();

  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.length() > 0) {
      shouldScroll = false;
      Serial.println("You: " + input);
      showLines("Thinking...", "");
      String answer = askGemini(input);
      Serial.println("ESP32: " + answer);
      startReplyDisplay(answer);
    }
  }

  updateScrollingLCD();
}