/* ESP-01 Telegram + Web + DHT22 + Relay
   Relay on GPIO0, DHT22 on GPIO2
   LOW = relay ON, HIGH = relay OFF
   Telegram is primary control, timers run in background.
*/

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266WebServer.h>
#include <UniversalTelegramBot.h>
#include "DHT.h"

// ----------------- Настройки -----------------
const char* WIFI_SSID = "WIFI_SSID";
const char* WIFI_PASSWORD = "WIFI_PASSWORD";
const String BOT_TOKEN = "BOT_TOKEN";


// Пины
const uint8_t RELAY_PIN = 0;   // GPIO0
const uint8_t DHTPIN    = 2;   // GPIO2
#define DHTTYPE DHT22

// Логика реле: false → LOW = ВКЛ, HIGH = ВЫКЛ
const bool RELAY_ACTIVE_HIGH = false;

// Интервалы
const unsigned long TELEGRAM_POLL_MS = 2000UL;
const unsigned long DHT_READ_MS = 5000UL;

// ----------------- Глобальные объекты -----------------
WiFiClientSecure secureClient;
UniversalTelegramBot bot(BOT_TOKEN, secureClient);
ESP8266WebServer server(80);
DHT dht(DHTPIN, DHTTYPE);

// ----------------- Состояния и таймеры -----------------
bool relayState = false; // логическое состояние (true = включено)
unsigned long relayAutoOffAt = 0; // millis(), 0 = нет авто-выключения
String relayAutoOffNotifyChat = ""; // chat_id для уведомления при авто-выключении

// Отложенное действие
bool delayedPending = false;
unsigned long delayedAt = 0;
enum DelayedType { DEL_NONE=0, DEL_ON=1, DEL_OFF=2, DEL_ON_WITH_DURATION=3 };
DelayedType delayedType = DEL_NONE;
unsigned long delayedDurationMin = 0; // если DEL_ON_WITH_DURATION
String delayedRequestChat = ""; // chat_id, откуда пришёл запрос (уведомлять потом)

// Временные метки
unsigned long lastTelegramPoll = 0;
unsigned long lastDhtRead = 0;

// ----------------- Вспомогательные функции -----------------
void relayApplyOutput(bool on) {
  // Преобразование логического состояния в физический уровень
  if (RELAY_ACTIVE_HIGH) {
    digitalWrite(RELAY_PIN, on ? HIGH : LOW);
  } else {
    digitalWrite(RELAY_PIN, on ? LOW : HIGH);
  }
  relayState = on;
}

void relayTurnOnMinutes(unsigned long minutes, const String &notifyChat = "") {
  relayApplyOutput(true);
  relayAutoOffAt = millis() + minutes * 60000UL;
  relayAutoOffNotifyChat = notifyChat;
}

void relayTurnOff(const String &notifyChat = "") {
  relayApplyOutput(false);
  relayAutoOffAt = 0;
  relayAutoOffNotifyChat = notifyChat;
}

// Шаблон безопасной проверки (millis overflow safe)
bool timePassed(unsigned long targetMillis) {
  if (targetMillis == 0) return false;
  return (long)(millis() - targetMillis) >= 0;
}

// Формируем статусный текст
String makeStatusText() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  String tempStr = isnan(t) ? String("N/A") : String(t, 1);
  String humStr  = isnan(h) ? String("N/A") : String((int)h);

  String s = "Температура: " + tempStr + "°C; Влажность: " + humStr + "%\n";
  s += "Реле: ";
  s += relayState ? "Включено\n" : "Выключено\n";

  if (relayState && relayAutoOffAt != 0) {
    long left_ms = (long)(relayAutoOffAt - millis());
    if (left_ms < 0) left_ms = 0;
    unsigned long left_min = (unsigned long)(left_ms / 60000UL);
    s += "Осталось: " + String(left_min) + " мин\n";
  }
  return s;
}

// ----------------- WEB-интерфейс -----------------
String webStatusHtml() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  String temp = isnan(t) ? "N/A" : String(t,1);
  String hum  = isnan(h) ? "N/A" : String((int)h);
  String html;
  html += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width'>";
  html += "<style>body{font-family:Arial;padding:12px;max-width:420px;margin:auto}button{padding:10px;margin-top:8px;width:100%}</style>";
  html += "<h2>ESP-01 Панель управления</h2>";
  html += "<p>Температура: " + temp + "°C<br>Влажность: " + hum + "%<br>Реле: " + String(relayState ? "Включено" : "Выключено") + "</p>";
  if (relayState && relayAutoOffAt) {
    long left_ms = (long)(relayAutoOffAt - millis()); if (left_ms<0) left_ms=0;
    html += "<p>Осталось: " + String(left_ms/60000) + " мин</p>";
  }
  html += "<form action='/on' method='GET'>Включить на <input name='min' type='number' value='10' min='1' style='width:70px'> минут<button type='submit'>Включить</button></form>";
  html += "<form action='/off' method='GET'><button type='submit'>Выключить</button></form>";
  html += "</html>";
  return html;
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", webStatusHtml());
}

void handleWebOn() {
  if (server.hasArg("min")) {
    int m = server.arg("min").toInt();
    if (m <= 0) m = 1;
    relayTurnOnMinutes((unsigned long)m);
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  server.send(200, "text/plain", "Specify ?min=N");
}

void handleWebOff() {
  relayTurnOff();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

// ----------------- TELEGRAM-парсер команд -----------------
void scheduleDelayed(int minutesBefore, DelayedType type, unsigned long durationMin, const String &chat_id) {
  delayedPending = true;
  delayedAt = millis() + (unsigned long)minutesBefore * 60000UL;
  delayedType = type;
  delayedDurationMin = durationMin;
  delayedRequestChat = chat_id;
}

void processTelegramMessage(const String &chat_id, const String &text) {
  String t = text;
  t.trim();
  if (t.length() == 0) return;

  // /status
  if (t == "/status") {
    bot.sendMessage(chat_id, makeStatusText(), "");
    return;
  }

  // если просто число (в минутах)
  bool allDigits = true;
  for (size_t i=0;i<t.length();++i) if (!isDigit(t[i])) { allDigits=false; break; }
  if (allDigits) {
    int v = t.toInt();
    if (v == 0) {
      relayTurnOff(chat_id);
      bot.sendMessage(chat_id, "Реле выключено", "");
    } else {
      relayTurnOnMinutes((unsigned long)v, chat_id);
      bot.sendMessage(chat_id, "Реле включено на " + String(v) + " мин", "");
    }
    return;
  }

  // если команда вида /10on10 или /10on или /10off
  if (t.startsWith("/")) {
    String cmd = t.substring(1); // убираем '/'
    int idxOn = cmd.indexOf("on");
    int idxOff = cmd.indexOf("off");

    if (idxOn > 0) {
      String before = cmd.substring(0, idxOn);
      String after = cmd.substring(idxOn+2);
      int minutesBefore = before.toInt();
      if (minutesBefore <= 0) {
        bot.sendMessage(chat_id, "Неправильный формат команды", "");
        return;
      }
      int duration = after.toInt();
      if (duration <= 0) duration = minutesBefore; // default
      scheduleDelayed(minutesBefore, DEL_ON_WITH_DURATION, (unsigned long)duration, chat_id);
      bot.sendMessage(chat_id, "Таймер: через " + String(minutesBefore) + " мин включить на " + String(duration) + " мин.", "");
      return;
    }

    if (idxOff > 0) {
      String before = cmd.substring(0, idxOff);
      int minutesBefore = before.toInt();
      if (minutesBefore <= 0) {
        bot.sendMessage(chat_id, "Неправильный формат команды", "");
        return;
      }
      scheduleDelayed(minutesBefore, DEL_OFF, 0, chat_id);
      bot.sendMessage(chat_id, "Таймер: через " + String(minutesBefore) + " мин выключить.", "");
      return;
    }

    // /onN и /off
    if (t.startsWith("/on")) {
      int n = 0;
      if (t.length() > 3) n = t.substring(3).toInt();
      if (n <= 0) {
        relayApplyOutput(true);
        relayAutoOffAt = 0;
        relayAutoOffNotifyChat = chat_id;
        bot.sendMessage(chat_id, "Реле включено (без таймера).", "");
      } else {
        relayTurnOnMinutes((unsigned long)n, chat_id);
        bot.sendMessage(chat_id, "Реле включено на " + String(n) + " мин.", "");
      }
      return;
    }

    if (t.startsWith("/off")) {
      relayTurnOff(chat_id);
      bot.sendMessage(chat_id, "Реле выключено.", "");
      return;
    }
  }

  bot.sendMessage(chat_id, "Неизвестная команда.", "");
}

// ----------------- Обработка фона: авто-выключение и отложенные действия -----------------
void handleAutoOffBackground() {
  if (relayState && relayAutoOffAt != 0) {
    if ( timePassed(relayAutoOffAt) ) {
      String notify = relayAutoOffNotifyChat;
      relayTurnOff();
      if (notify.length()) bot.sendMessage(notify, "Авто-выключение: реле выключено.", "");
    }
  }
}

void handleDelayedBackground() {
  if (!delayedPending) return;
  if ( timePassed(delayedAt) ) {
    String notify = delayedRequestChat;
    if (delayedType == DEL_OFF) {
      relayTurnOff(notify);
      if (notify.length()) bot.sendMessage(notify, "Таймер выполнен: реле выключено.", "");
    } else if (delayedType == DEL_ON_WITH_DURATION) {
      relayTurnOnMinutes(delayedDurationMin, notify);
      if (notify.length()) bot.sendMessage(notify, "Таймер выполнен: включено на " + String(delayedDurationMin) + " мин.", "");
    } else if (delayedType == DEL_ON) {
      relayApplyOutput(true);
      if (notify.length()) bot.sendMessage(notify, "Таймер выполнен: реле включено.", "");
    }
    delayedPending = false;
    delayedType = DEL_NONE;
    delayedRequestChat = "";
  }
}

// ----------------- Telegram poll ----------
void pollTelegram() {
  if (millis() - lastTelegramPoll < TELEGRAM_POLL_MS) return;
  lastTelegramPoll = millis();
  int numNew = bot.getUpdates(bot.last_message_received + 1);
  if (numNew > 0) {
    for (int i = 0; i < numNew; ++i) {
      String chat_id = bot.messages[i].chat_id;
      String text = bot.messages[i].text;
      processTelegramMessage(chat_id, text);
    }
  }
}

// ----------------- setup / loop -----------------
void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  relayApplyOutput(false); // выключаем по-умолчанию
  dht.begin();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  secureClient.setInsecure();
  Serial.print("Connecting WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000UL) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else Serial.println("WiFi connection failed");

  server.on("/", handleRoot);
  server.on("/on", handleWebOn);
  server.on("/off", handleWebOff);
  server.begin();
}

void loop() {
  server.handleClient();
  if (millis() - lastDhtRead >= DHT_READ_MS) {
    lastDhtRead = millis();
    dht.readTemperature();
    dht.readHumidity();
  }
  pollTelegram();
  handleDelayedBackground();
  handleAutoOffBackground();
  delay(10);
}

