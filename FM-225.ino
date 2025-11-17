#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <time.h>
// ===== Добавлено для RTC =====
#include <RTClib.h> // Убедитесь, что версия 2.1.4 или новее

RTC_DS3231 rtc; // Создаем объект RTC для использования стандартного Wire (I2C0: GPIO 21, 22)
// =============================

// ===== Wi‑Fi =====
const char* ssid     = "SSID";
const char* password = "PASS";
// ===== Пароль для очистки журнала =====
const char* CLEAR_LOG_PW = "admin123";
// ===== Serial2 для модуля распознавания =====
#define FACE_SERIAL Serial2
const int RX_PIN    = 16;
const int TX_PIN    = 17; // TX_PIN теперь 17, так как LED_FAIL перенесен
const uint32_t BAUD = 115200;
// ===== Веб‑сервер =====
AsyncWebServer server(80);
// ===== Кнопка и светодиоды (active LOW) =====
const int BUTTON_PIN = 4;
const int LED_BUSY   = 5;
const int LED_OK     = 18;
const int LED_FAIL   = 13; 
// ===== Журнал разблокировок =====
struct UnlockEntry {
  String tsString;   // сохраняем строку отметки времени
  uint16_t uid;
  String name;
};
UnlockEntry unlockLog[100];
int logHead = 0, logCount = 0;
// ===== Дебаунс =====
bool lastButtonState           = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;
// ===== Для enroll =====
String lastEnrollName;
// ===== Словарь ответов модуля =====
const char* MR_RESULTS[] = {
  "Операция успешна","Модуль отклонил команду","Процесс прерван",nullptr,
  "Не удалось открыть камеру","Неизвестная ошибка","Неверный параметр",
  "Недостаточно памяти","Пользователь не найден","Превышено число пользователей",
  "Лицо уже зарегистрировано",nullptr,nullptr,
  "Проверка на живое лицо не пройдена","Время ожидания истекло","Неудачная авторизация",
  nullptr,nullptr,nullptr,
  "Не удалось прочитать файл","Не удалось записать файл","Протокол не зашифрован",
  nullptr,"RGB-изображение не готово",nullptr,"Фото слишком большое","Фото слишком маленькое"
};
// ===== Проверка имени “текст-число” ===== 
bool isValidName(const String& name) {
  int dash = name.lastIndexOf('-');
  if (dash <= 0 || dash >= name.length()-1) return false;
  String idPart = name.substring(dash+1);
  for (char c: idPart) if (!isDigit(c)) return false;
  return idPart.toInt()>0;
}
// ===== Сохранение/загрузка журнала в SPIFFS =====
void saveUnlockLogFS() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  int start = (logHead - logCount + 100) % 100;
  for (int i=0; i<logCount; i++) {
    auto& e = unlockLog[(start+i)%100];
    JsonObject o = arr.createNestedObject();
    o["ts"]   = e.tsString;
    o["uid"]  = e.uid;
    o["name"] = e.name;
  }
  File tmp = SPIFFS.open("/unlocklog.tmp", FILE_WRITE);
  if (!tmp) return;
  serializeJson(arr, tmp);
  tmp.close();
  SPIFFS.remove("/unlocklog.json");
  SPIFFS.rename("/unlocklog.tmp", "/unlocklog.json");
}
void loadUnlockLogFS() {
  if (!SPIFFS.exists("/unlocklog.json")) return;
  File f = SPIFFS.open("/unlocklog.json", FILE_READ);
  if (!f || f.size()==0) return;
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, f)==DeserializationError::Ok) {
    JsonArray arr = doc.as<JsonArray>();
    logCount = min(100, (int)arr.size());
    logHead  = logCount % 100;
    int idx = 0;
    for (JsonObject o : arr) {
      unlockLog[idx].tsString = o["ts"].as<String>();
      unlockLog[idx].uid      = o["uid"] | 0;
      unlockLog[idx].name     = o["name"].as<String>();
      idx++;
    }
  }
  f.close();
}
// ===== Утилиты =====
uint8_t getParity(const uint8_t* d, size_t l) {
  uint8_t p=0;
  for(size_t i=2;i<l;i++) p ^= d[i];
  return p;
}
void sendPacket(const uint8_t* d, size_t l) { FACE_SERIAL.write(d,l); }

// ===== Получение времени из RTC =====
String getCurrentTimeString() {
    char buf[20] = "0000-00-00 00:00:00";
    DateTime now = rtc.now(); // Получаем время из RTC через стандартный Wire

    // Проверка валидности времени RTC
    if (now.year() >= 2020 && now.year() < 2100) {
        // Форматируем строку времени
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                 now.year(), now.month(), now.day(),
                 now.hour(), now.minute(), now.second());
        // Serial.println("Time obtained from RTC.");
    } else {
        Serial.println("RTC time seems invalid.");
    }
    return String(buf);
}

// ===== Добавляем запись: используем время из RTC =====
void addUnlockLog(uint16_t uid, const String& name){
  // ВСЕГДА получаем время из RTC
  unlockLog[logHead].tsString = getCurrentTimeString();
  unlockLog[logHead].uid  = uid;
  unlockLog[logHead].name = name;
  logHead = (logHead + 1) % 100;
  if (logCount < 100) logCount++;
  saveUnlockLogFS();
}

// ===== Конструкторы команд =====
std::vector<uint8_t> makeUnlock(){
  std::vector<uint8_t> p;
  uint16_t sync=0xEFAA, sz=2;
  p.push_back(sync>>8); p.push_back(sync&0xFF);
  p.push_back(0x12);
  p.push_back(sz>>8); p.push_back(sz&0xFF);
  p.push_back(0); p.push_back(5);
  p.push_back(getParity(p.data(), p.size()));
  return p;
}
std::vector<uint8_t> makeEnroll(const String& name){
  std::vector<uint8_t> p;
  uint16_t sync=0xEFAA, sz=35;
  p.push_back(sync>>8); p.push_back(sync&0xFF);
  p.push_back(0x1D);
  p.push_back(sz>>8); p.push_back(sz&0xFF);
  p.push_back(0);
  for(int i=0;i<32;i++) p.push_back(i<name.length()?name[i]:0);
  p.push_back(0); p.push_back(0);
  p.push_back(getParity(p.data(), p.size()));
  return p;
}
std::vector<uint8_t> makeDeleteUser(uint16_t uid){
  std::vector<uint8_t> p;
  uint16_t sync=0xEFAA, sz=2;
  p.push_back(sync>>8); p.push_back(sync&0xFF);
  p.push_back(0x20);
  p.push_back(sz>>8); p.push_back(sz&0xFF);
  p.push_back(uid>>8); p.push_back(uid&0xFF);
  p.push_back(getParity(p.data(), p.size()));
  return p;
}
// ===== HTML-интерфейс =====
const char indexHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Face Control</title>
<style>
  body {
    font-family: Arial, sans-serif;
    background: #eef2f7;
    color: #333;
    margin: 0;
    padding: 0;
  }
  header {
    background: #0078d7;
    color: white;
    padding: 1rem;
    text-align: center;
    font-size: 1.5rem;
    font-weight: bold;
    box-shadow: 0 2px 6px rgba(0,0,0,0.15);
    position: sticky;
    top: 0;
    z-index: 10;
  }
  main {
    max-width: 900px;
    margin: auto;
    padding: 1rem;
  }
  .card {
    background: white;
    padding: 1rem 1.2rem;
    border-radius: 10px;
    margin-bottom: 1.5rem;
    box-shadow: 0 4px 12px rgba(0,0,0,0.08);
  }
  h2 {
    margin-top: 0;
    font-size: 1.3rem;
    color: #0078d7;
    border-bottom: 2px solid #e5eaf0;
    padding-bottom: .4rem;
  }
  .controls {
    display: flex;
    flex-wrap: wrap;
    gap: .5rem;
    margin-top: .5rem;
  }
  input {
    flex: 1;
    padding: .5rem;
    font-size: 1rem;
    border: 1px solid #ccc;
    border-radius: 6px;
    min-width: 120px;
  }
  button {
    padding: .5rem 1rem;
    font-size: 1rem;
    background: #0078d7;
    color: white;
    border: none;
    border-radius: 6px;
    cursor: pointer;
    transition: background 0.2s, transform 0.1s;
  }
  button:hover {
    background: #005ea6;
  }
  button:active {
    transform: scale(0.97);
  }
  #rtc-time {
    font-weight: bold;
    color: #222;
    font-size: 1.4rem;
    padding-top: .5rem;
  }
  table {
    width: 100%;
    border-collapse: collapse;
    margin-top: .5rem;
    font-size: 0.95rem;
  }
  th, td {
    padding: .6rem;
    border-bottom: 1px solid #e0e0e0;
    text-align: left;
  }
  th {
    background: #f7f9fb;
    font-weight: 600;
  }
  tbody tr:hover {
    background: #f2f7ff;
  }
</style>
</head>
<body>
<header>Система распознавания</header>
<main>
  <div class="card">
    <h2>Текущее время (RTC)</h2>
    <div id="rtc-time">Загрузка...</div>
    <button onclick="syncRtcTime()" style="margin-top: 10px; padding: 0.5rem 1rem; font-size: 0.9rem;">Обновить время</button>
  </div>

  <div class="card">
    <h2>Пользователи</h2>
    <div class="controls">
      <input id="username" placeholder="Имя">
      <button onclick="enroll()">Регистрация</button>
      <input id="delid" placeholder="UID" style="max-width:80px">
      <button onclick="deluser()">Удалить</button>
    </div>
  </div>

  <div class="card">
    <h2>Распознать</h2>
    <button id="btnUnlock" onclick="doUnlock()">Старт</button>
  </div>

  <div class="card">
    <h2>Журнал</h2>
    <button onclick="clearLog()">Очистить журнал</button>
    <table id="log">
      <thead>
        <tr><th>Время</th><th>UID</th><th>Имя</th></tr>
      </thead>
      <tbody></tbody>
    </table>
  </div>
</main>

<script>
  function enroll() {
    const u = encodeURIComponent(document.getElementById('username').value);
    fetch(`/enroll?username=${u}`);
  }
  function deluser() {
    const i = encodeURIComponent(document.getElementById('delid').value);
    fetch(`/deluser?id=${i}`);
  }
  function doUnlock() {
    document.getElementById('btnUnlock').disabled = true;
    fetch('/unlock').finally(()=>setTimeout(()=>document.getElementById('btnUnlock').disabled=false,500));
  }
  function loadLog() {
    fetch('/log').then(r=>r.json()).then(arr=>{
      const tb=document.querySelector('#log tbody'); tb.innerHTML='';
      arr.forEach(e=>{
        const name=e.name||'—';
        tb.insertAdjacentHTML('beforeend',`<tr><td>${e.ts}</td><td>${e.uid}</td><td>${name}</td></tr>`);
      });
    });
  }
  function clearLog() {
    const pw=prompt('Пароль:');
    if(pw)fetch('/clearlog',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:`pw=${encodeURIComponent(pw)}`})
      .then(r=>r.text()).then(alert);
  }
  function updateRtcTime() {
    fetch('/rtctime').then(r => r.text()).then(timeStr => {
      document.getElementById('rtc-time').textContent = timeStr;
    }).catch(err => {
      document.getElementById('rtc-time').textContent = 'Ошибка';
    });
  }
  function syncRtcTime() {
    fetch('/synctime', {method: 'POST'})
      .then(r => r.text())
      .then(response => {
        alert(response); // Показываем результат (успешно/ошибка)
        updateRtcTime(); // Обновляем отображаемое время
      })
      .catch(err => {
        console.error('Error syncing RTC time:', err);
        alert('Ошибка синхронизации времени');
      });
  }
  setInterval(loadLog, 3000);
  setInterval(updateRtcTime, 1000);
  window.addEventListener('load', () => {
    loadLog();
    updateRtcTime();
  });
</script>
</body>
</html>
)rawliteral";


// ===== Парсер ответов =====
enum { S1,S2,ST,S3,S4,SB,SC };
int ps=S1, blen=0, bi=0;
uint8_t hbuf[3], bodyBuf[256];

// ===== Инициализация RTC (новая функция) =====
void setupRTC() {
  Serial.println("Initializing RTC on default I2C (GPIO 21/22)...");
  // Wire.begin() вызывается автоматически внутри rtc.begin(), если не вызвано ранее.
  // rtc.begin() использует стандартный Wire (I2C0: GPIO 21 - SDA, GPIO 22 - SCL)
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC on default I2C!");
    // Можно остановить выполнение или продолжить без RTC
    // while (1);
  } else {
    Serial.println("RTC found on default I2C.");
    if (rtc.lostPower()) {
      Serial.println("RTC lost power. Time might be incorrect.");
      // RTC может быть настроен позже через NTP, если Wi-Fi подключится
      // (Но RTC теперь основной источник, NTP используется для коррекции)
    } else {
      DateTime now = rtc.now();
      Serial.print("RTC Time: ");
      Serial.print(now.year(), DEC);
      Serial.print('/');
      Serial.print(now.month(), DEC);
      Serial.print('/');
      Serial.print(now.day(), DEC);
      Serial.print(" ");
      Serial.print(now.hour(), DEC);
      Serial.print(':');
      Serial.print(now.minute(), DEC);
      Serial.print(':');
      Serial.print(now.second(), DEC);
      Serial.println();
    }
  }
}

// ===== Синхронизация RTC с NTP  =====
void syncRTCfromNTP() {
    // Пытаемся получить время через NTP
    struct tm ti;
    unsigned long startWait = millis();
    const unsigned long timeout = 10000; // 10 секунд таймаут

    Serial.println("Attempting to sync RTC from NTP...");

    while (!getLocalTime(&ti) && (millis() - startWait < timeout)) {
      delay(100);
    }

    if (getLocalTime(&ti)) {
        Serial.println("NTP time obtained.");
        // Преобразуем tm в DateTime для RTC
        DateTime ntpTime(ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour, ti.tm_min, ti.tm_sec);

        // Получаем текущее время RTC для сравнения
        DateTime rtcNow = rtc.now();

        // Простая проверка: если RTC год выглядит неправильно или NTP время новее
        if (rtcNow.year() < 2020 || rtcNow.year() > 2100 || rtcNow < ntpTime) {
            Serial.println("Setting/Updating RTC from NTP...");
            rtc.adjust(ntpTime);
            DateTime updatedRtcTime = rtc.now();
            Serial.print("RTC Time after update: ");
            Serial.print(updatedRtcTime.year(), DEC);
            Serial.print('/');
            Serial.print(updatedRtcTime.month(), DEC);
            Serial.print('/');
            Serial.print(updatedRtcTime.day(), DEC);
            Serial.print(" ");
            Serial.print(updatedRtcTime.hour(), DEC);
            Serial.print(':');
            Serial.print(updatedRtcTime.minute(), DEC);
            Serial.print(':');
            Serial.print(updatedRtcTime.second(), DEC);
            Serial.println();
        } else {
            // Проверяем разницу во времени (в секундах)
            int32_t timeDiff = ntpTime.unixtime() - rtcNow.unixtime();
            // Если разница больше 5 минут (300 секунд) или меньше -5 минут (-300 секунд)
            if (abs(timeDiff) > 300) {
                Serial.println("RTC time differs significantly from NTP. Updating...");
                rtc.adjust(ntpTime);
                DateTime updatedRtcTime = rtc.now();
                Serial.print("RTC Time after update: ");
                Serial.print(updatedRtcTime.year(), DEC);
                Serial.print('/');
                Serial.print(updatedRtcTime.month(), DEC);
                Serial.print('/');
                Serial.print(updatedRtcTime.day(), DEC);
                Serial.print(" ");
                Serial.print(updatedRtcTime.hour(), DEC);
                Serial.print(':');
                Serial.print(updatedRtcTime.minute(), DEC);
                Serial.print(':');
                Serial.print(updatedRtcTime.second(), DEC);
                Serial.println();
            } else {
                Serial.println("RTC time is up-to-date with NTP.");
            }
        }
    } else {
        Serial.println("Failed to get NTP time for RTC sync.");
    }
}

void setup(){
  Serial.begin(115200);
  Serial.println("Starting...");

  // --- Инициализация RTC ---
  setupRTC();
  // --------------------------

  FACE_SERIAL.begin(BAUD, SERIAL_8N1, RX_PIN, TX_PIN);

  // SPIFFS
  if(!SPIFFS.begin()) Serial.println("SPIFFS mount failed");
  Serial.println("SPIFFS mounted.");

  // GPIO
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUSY,   OUTPUT);
  pinMode(LED_OK,     OUTPUT);
  pinMode(LED_FAIL,   OUTPUT); // Теперь на GPIO 13
  digitalWrite(LED_BUSY, HIGH);
  digitalWrite(LED_OK,   HIGH);
  digitalWrite(LED_FAIL, HIGH); // Теперь на GPIO 13

  // Wi‑Fi события: переподключение и NTP после reconnect
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t){
    Serial.println("WiFi lost, reconnecting...");
    WiFi.reconnect();
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t){
    Serial.print("Reconnected, IP: ");
    Serial.println(WiFi.localIP());
    // Настройка NTP при переподключении
    configTzTime("UTC-3","pool.ntp.org","time.nist.gov");
    // Синхронизируем RTC с NTP
    syncRTCfromNTP();
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);

  // Подключаемся к Wi‑Fi
  WiFi.begin(ssid,password);
  Serial.print("Connecting WiFi");
  while(WiFi.status()!=WL_CONNECTED){
    digitalWrite(LED_BUSY,LOW); delay(250);
    digitalWrite(LED_BUSY,HIGH);delay(250);
    Serial.print(".");
  }
  digitalWrite(LED_BUSY,HIGH);
  Serial.println(" IP: "+WiFi.localIP().toString());

  // Первоначальная настройка NTP и синхронизация RTC
  configTzTime("UTC-3","pool.ntp.org","time.nist.gov");
  syncRTCfromNTP(); // Попытка синхронизации RTC при старте

  // Загружаем существующий журнал (только строки)
  loadUnlockLogFS();

  // HTTP‑эндпойнты
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send_P(200,"text/html",indexHtml);
  });

  // Новый эндпоинт для получения времени RTC
  server.on("/rtctime", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send(200, "text/plain", getCurrentTimeString());
  });

  // НОВЫЙ ЭНДПОИНТ ДЛЯ СИНХРОНИЗАЦИИ ВРЕМЕНИ
  server.on("/synctime", HTTP_POST, [](AsyncWebServerRequest* r){
    if (WiFi.status() == WL_CONNECTED) {
      syncRTCfromNTP(); // Вызываем вашу функцию синхронизации
      r->send(200, "text/plain", "Время RTC обновлено из NTP");
    } else {
      r->send(500, "text/plain", "Нет подключения к Wi-Fi");
    }
  });

  server.on("/unlock", HTTP_GET, [](AsyncWebServerRequest* r){
    auto p=makeUnlock(); sendPacket(p.data(),p.size());
    digitalWrite(LED_BUSY,LOW);
    r->send(200,"text/plain","OK");
  });
  server.on("/enroll", HTTP_GET, [](AsyncWebServerRequest* r){
    if(!r->hasParam("username")){ r->send(400,"text/plain","no name"); return; }
    lastEnrollName=r->getParam("username")->value();
    auto p=makeEnroll(lastEnrollName); sendPacket(p.data(),p.size());
    digitalWrite(LED_BUSY,LOW);
    r->send(200,"text/plain","OK");
  });
  server.on("/deluser", HTTP_GET, [](AsyncWebServerRequest* r){
    if(!r->hasParam("id")){ r->send(400,"text/plain","no id"); return; }
    uint16_t uid=r->getParam("id")->value().toInt();
    auto p=makeDeleteUser(uid); sendPacket(p.data(),p.size());
    digitalWrite(LED_BUSY,LOW);
    r->send(200,"text/plain","OK");
  });
  server.on("/log", HTTP_GET, [](AsyncWebServerRequest* r){
    DynamicJsonDocument d(4096);
    JsonArray arr=d.to<JsonArray>();
    int st=(logHead-logCount+100)%100;
    for(int i=0;i<logCount;i++){
      auto& e=unlockLog[(st+i)%100];
      JsonObject o=arr.createNestedObject();
      o["ts"]=e.tsString;
      o["uid"]=e.uid;
      o["name"]=e.name;
    }
    String out; serializeJson(arr,out);
    r->send(200,"application/json",out);
  });
  server.on("/clearlog", HTTP_POST, [](AsyncWebServerRequest* r){
    if(!r->hasParam("pw",true)||r->getParam("pw",true)->value()!=CLEAR_LOG_PW){
      r->send(403,"text/plain","Forbidden"); return;
    }
    logCount=logHead=0;
    SPIFFS.remove("/unlocklog.json");
    r->send(200,"text/plain","Cleared");
  });
  server.begin();
}

void loop(){
  // Кнопка разблокировки
  int reading=digitalRead(BUTTON_PIN);
  if(reading!=lastButtonState) lastDebounceTime=millis();
  if((millis()-lastDebounceTime>debounceDelay)&&reading==LOW){
    auto p=makeUnlock(); sendPacket(p.data(),p.size());
    digitalWrite(LED_BUSY,LOW); delay(200);
  }
  lastButtonState=reading;
  // Чтение ответов модуля
  while(FACE_SERIAL.available()){
    uint8_t b=FACE_SERIAL.read();
    switch(ps){
      case S1: if(b==0xEF) ps=S2; break;
      case S2: ps=(b==0xAA?ST:S1); break;
      case ST: hbuf[0]=b; ps=S3; break;
      case S3: hbuf[1]=b; ps=S4; break;
      case S4:
        hbuf[2]=b; blen=(hbuf[1]<<8)|hbuf[2];
        bi=0; ps=(blen<=sizeof(bodyBuf)?SB:S1);
        break;
      case SB:
        bodyBuf[bi++]=b; if(bi>=blen) ps=SC; break;
      case SC:{
        uint8_t mid=bodyBuf[0], res=bodyBuf[1];
        if(mid==0x12||mid==0x1D) digitalWrite(LED_BUSY,HIGH);
        if(mid==0x12){ // unlock
          if(res==0&&blen>=4){
            uint16_t uid=(bodyBuf[2]<<8)|bodyBuf[3];
            char tmp[33]; memcpy(tmp,&bodyBuf[4],32); tmp[32]=0;
            String name(tmp); name.trim();
            if(isValidName(name)){
              addUnlockLog(uid,name); // Теперь использует RTC
              digitalWrite(LED_OK,LOW);delay(200);digitalWrite(LED_OK,HIGH);
            } else {
              Serial.printf("Invalid name '%s' for UID=%u\n",name.c_str(),uid);
              digitalWrite(LED_FAIL,LOW);delay(200);digitalWrite(LED_FAIL,HIGH); // Теперь на GPIO 13
            }
          } else {
            digitalWrite(LED_FAIL,LOW);delay(200);digitalWrite(LED_FAIL,HIGH); // Теперь на GPIO 13
          }
        }
        else if(mid==0x20){ // delete
          if(res==0){digitalWrite(LED_OK,LOW);delay(200);digitalWrite(LED_OK,HIGH);}
          else       {digitalWrite(LED_FAIL,LOW);delay(200);digitalWrite(LED_FAIL,HIGH);} // Теперь на GPIO 13
        }
        else if(mid==0x1D){ // enroll
          if(res==0&&blen>=4){
            uint16_t uid=(bodyBuf[2]<<8)|bodyBuf[3];
            Serial.printf("Enroll OK UID=%u Name=%s\n",uid,lastEnrollName.c_str());
            digitalWrite(LED_OK,LOW);delay(200);digitalWrite(LED_OK,HIGH);
          } else {
            digitalWrite(LED_FAIL,LOW);delay(200);digitalWrite(LED_FAIL,HIGH); // Теперь на GPIO 13
          }
        }
        ps=S1;
      } break;
    }
  }
}
