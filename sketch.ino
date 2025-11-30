#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>

const char* ssid = "ssid_name";
const char* password = "pass_wifi";

#define RELAY_PIN 0  // GPIO2 ESP-01

String BOTtoken = "token_telegram_bot";  // токен бота

WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

unsigned long bot_lasttime;
const int bot_mtbs = 2000;  // опрос Telegram каждые 2 сек

// ---- Управление реле ----
unsigned long relayOnTime = 0;
unsigned long relayDuration = 0;   // время работы в миллисекундах
bool relayIsOn = false;

void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);

  client.setInsecure(); // Telegram API

  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  Serial.println(WiFi.localIP());
}

void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = bot.messages[i].chat_id;
    String text = bot.messages[i].text;

    Serial.println("Received: " + text);

    // Проверяем, что это число
    bool isNumber = true;
    for (int c = 0; c < text.length(); c++) {
      if (!isDigit(text[c])) isNumber = false;
    }

    if (!isNumber) {
      bot.sendMessage(chat_id, "Отправьте число: 0 для выключения, >0 — чтобы включить на N минут", "");
      return;
    }

    int value = text.toInt();

    // ====== ВЫКЛЮЧЕНИЕ ======
    if (value == 0) {
      digitalWrite(RELAY_PIN, HIGH);
      relayIsOn = false;
      bot.sendMessage(chat_id, "Реле выключено", "");
    }

    // ====== ВКЛЮЧЕНИЕ НА value МИНУТ ======
    else {
      relayDuration = (unsigned long)value * 60UL * 1000UL;  // минуты → миллисекунды
      relayOnTime = millis();
      relayIsOn = true;

      digitalWrite(RELAY_PIN, LOW);

      bot.sendMessage(chat_id,
        "Реле включено на " + String(value) + " минут",
        ""
      );
    }
  }
}

void loop() {
  // --- Telegram Bot ---
  if (millis() - bot_lasttime > bot_mtbs) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

    if (numNewMessages) {
      handleNewMessages(numNewMessages);
    }

    bot_lasttime = millis();
  }

  // --- Авто-выключение по времени ---
  if (relayIsOn && (millis() - relayOnTime >= relayDuration)) {
    digitalWrite(RELAY_PIN, HIGH);
    relayIsOn = false;
    Serial.println("Auto OFF (timer expired)");
  }
}


