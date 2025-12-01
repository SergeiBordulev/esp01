
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include "DHT.h"

#define WIFI_SSID      "ssid"
#define WIFI_PASSWORD  "pass"

#define BOT_TOKEN "token_bot"

#define RELAY_PIN 0
#define DHTPIN    2
#define DHTTYPE   DHT22

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

DHT dht(DHTPIN, DHTTYPE);

unsigned long lastCheckTime = 0;
unsigned long relayOffTime = 0;
bool relayState = false;

// Отложенные действия
bool delayedActionPending = false;
unsigned long delayedActionTime = 0;
bool delayedActionTypeOn = false;
unsigned long delayedOnDuration = 0;

void relayOnMinutes(unsigned long minutes) {
  relayState = true;
  digitalWrite(RELAY_PIN, LOW);  // РЕЛЕ ВКЛЮЧЕНО (как ты просил)
  relayOffTime = millis() + minutes * 60000UL;
}

void relayOff() {
  relayState = false;
  digitalWrite(RELAY_PIN, HIGH);   // РЕЛЕ ВЫКЛЮЧЕНО
  relayOffTime = 0;
}

void sendStatus(String chat_id) {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  String temp = String(isnan(t) ? -1000 : t, 1);
  String hum  = String(isnan(h) ? -1000 : h, 0);

  String msg =
    "Temp: " + temp + "°C; Hum: " + hum + "%\n" +
    "Raley: " + String(relayState ? "Включено" : "Выключено") + "\n";

  if (relayState) {
    long left_ms = relayOffTime - millis();
    if (left_ms < 0) left_ms = 0;
    msg += "Осталось: " + String(left_ms / 60000) + " мин\n";
  }

  bot.sendMessage(chat_id, msg, "");
}

void checkAutoOff(String chat_id) {
  if (relayState && relayOffTime > 0 && millis() >= relayOffTime) {
    relayOff();
    bot.sendMessage(chat_id, "Авто-выключение: реле выключено.", "");
  }
}

void checkDelayedActions(String chat_id) {
  if (delayedActionPending && millis() >= delayedActionTime) {
    delayedActionPending = false;

    if (delayedActionTypeOn) {
      relayOnMinutes(delayedOnDuration);
      bot.sendMessage(chat_id,
        "Таймер: включено на " + String(delayedOnDuration) + " минут.", "");
    } else {
      relayOff();
      bot.sendMessage(chat_id, "Таймер: реле выключено.", "");
    }
  }
}

void parseCommand(String text, String chat_id) {
  text.trim();

  // /status
  if (text == "/status") {
    sendStatus(chat_id);
    return;
  }

  // Число > 0 → включение на N минут
  if (text.toInt() > 0) {
    unsigned long minutes = text.toInt();
    relayOnMinutes(minutes);
    bot.sendMessage(chat_id, "Реле включено на " + String(minutes) + " минут.", "");
    return;
  }

  // 0 → выключить
  if (text == "0") {
    relayOff();
    bot.sendMessage(chat_id, "Реле выключено.", "");
    return;
  }

  // Отложенные команды
  if (text.startsWith("/")) {
    String t = text.substring(1);

    int idxOn = t.indexOf("on");
    int idxOff = t.indexOf("off");

    // /10on10  or  /10on
    if (idxOn > 0) {
      String before = t.substring(0, idxOn);
      String after = t.substring(idxOn + 2);

      unsigned long delayMin = before.toInt();
      if (delayMin == 0) return;

      unsigned long duration = after.toInt();
      if (duration == 0) duration = delayMin; // default

      delayedActionPending = true;
      delayedActionTypeOn = true;
      delayedOnDuration = duration;
      delayedActionTime = millis() + delayMin * 60000UL;

      bot.sendMessage(chat_id,
        "Таймер: через " + String(delayMin) +
        " минут включить на " + String(duration) + " минут.",
        "");
      return;
    }

    // /10off
    if (idxOff > 0) {
      String before = t.substring(0, idxOff);
      unsigned long delayMin = before.toInt();
      if (delayMin == 0) return;

      delayedActionPending = true;
      delayedActionTypeOn = false;
      delayedActionTime = millis() + delayMin * 60000UL;

      bot.sendMessage(chat_id,
        "Таймер: через " + String(delayMin) + " минут выключить.",
        "");
      return;
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);  // по умолчанию реле ВЫКЛЮЧЕНО

  dht.begin();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  client.setInsecure();

  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
  }
}

void loop() {
  if (millis() - lastCheckTime > 2000) {
    lastCheckTime = millis();

    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

    while (numNewMessages) {
      for (int i = 0; i < numNewMessages; i++) {
        String chat_id = bot.messages[i].chat_id;
        String text    = bot.messages[i].text;
        parseCommand(text, chat_id);
        checkDelayedActions(chat_id);
        checkAutoOff(chat_id);
      }

      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
  }
}

