#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <time.h>

// ===== Wi‑Fi =====
const char* ssid     = "vzvt2";
const char* password = "rrr12345678";

// ===== Пароль для очистки журнала =====
const char* CLEAR_LOG_PW = "admin123";

// ===== Serial2 для модуля распознавания =====
#define FACE_SERIAL Serial2
const int RX_PIN    = 16;
const int TX_PIN    = 17;
const uint32_t BAUD = 115200;

// ===== Веб‑сервер =====
AsyncWebServer server(80);

// ===== Кнопка и светодиоды (active LOW) =====
const int BUTTON_PIN = 4;
const int LED_BUSY   = 5;
const int LED_OK     = 18;
const int LED_FAIL   = 19;

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

// Добавляем запись: формируем строку времени и сохраняем
void addUnlockLog(uint16_t uid, const String& name){
  // получаем локальное время
  struct tm tm_info;
  if (getLocalTime(&tm_info)) {
    char buf[20];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    unlockLog[logHead].tsString = String(buf);
  } else {
    unlockLog[logHead].tsString = String("0000-00-00 00:00:00");
  }
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
<!DOCTYPE html><html><head>
  <meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Face Control</title>
  <style>
    body{font-family:sans-serif;background:#f4f7fa;color:#333;padding:20px;}
    .card{background:#fff;padding:1rem;border-radius:6px;margin-bottom:1rem;
          box-shadow:0 2px 5px rgba(0,0,0,0.1);}
    .controls{display:flex;gap:.5rem;flex-wrap:wrap;}
    input,button{padding:.5rem;font-size:1rem;}
    button{background:#0078d7;color:#fff;border:none;border-radius:4px;cursor:pointer;}
    table{width:100%;border-collapse:collapse;margin-top:1rem;}
    th,td{padding:.5rem;border-bottom:1px solid #ddd;}
    th{background:#e1e7ee;}
  </style>
</head><body>
  <h1>Система распознавания</h1>
  <div class="card">
    <h2>Пользователи</h2>
    <div class="controls">
      <input id="username" placeholder="Имя">
      <button onclick="enroll()">Регистрация</button>
      <input id="delid" placeholder="UID" style="width:6rem">
      <button onclick="deluser()">Удалить</button>
    </div>
  </div>
  <div class="card">
    <h2>Распознать</h2>
    <button id="btnUnlock" onclick="doUnlock()">Старт</button>
  </div>
  <div class="card">
    <h2>Журнал</h2>
    <button onclick="clearLog()">Очистить</button>
    <table id="log"><thead><tr><th>Время</th><th>UID</th><th>Имя</th></tr></thead><tbody></tbody></table>
  </div>
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
  setInterval(loadLog,3000);
  window.addEventListener('load',loadLog);
</script>
</body></html>
)rawliteral";

// ===== Парсер ответов =====
enum { S1,S2,ST,S3,S4,SB,SC };
int ps=S1, blen=0, bi=0;
uint8_t hbuf[3], bodyBuf[256];

void setup(){
  Serial.begin(115200);
  FACE_SERIAL.begin(BAUD, SERIAL_8N1, RX_PIN, TX_PIN);
  Serial.println("Starting...");
  // SPIFFS
  if(!SPIFFS.begin()) Serial.println("SPIFFS mount failed");
  Serial.println("SPIFS...");
  

  // GPIO
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUSY,   OUTPUT);
  pinMode(LED_OK,     OUTPUT);
  pinMode(LED_FAIL,   OUTPUT);
  digitalWrite(LED_BUSY, HIGH);
  digitalWrite(LED_OK,   HIGH);
  digitalWrite(LED_FAIL, HIGH);

  // Wi‑Fi события: переподключение и NTP после reconnect
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t){
    Serial.println("WiFi lost, reconnecting...");
    WiFi.reconnect();
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t){
    Serial.print("Reconnected, IP: ");
    Serial.println(WiFi.localIP());
    configTzTime("UTC-3","pool.ntp.org","time.nist.gov");
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
  Serial.println("\nIP: "+WiFi.localIP().toString());
  // Сначала настраиваем NTP, чтобы getLocalTime работал
  configTzTime("UTC-3","pool.ntp.org","time.nist.gov");
  struct tm ti;
  while(!getLocalTime(&ti)){
    delay(100);
  }
  Serial.println("NTP...");
  // Загружаем существующий журнал (только строки)
  loadUnlockLogFS();
  // HTTP‑эндпойнты
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send_P(200,"text/html",indexHtml);
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
              addUnlockLog(uid,name);
              digitalWrite(LED_OK,LOW);delay(200);digitalWrite(LED_OK,HIGH);
            } else {
              Serial.printf("Invalid name '%s' for UID=%u\n",name.c_str(),uid);
              digitalWrite(LED_FAIL,LOW);delay(200);digitalWrite(LED_FAIL,HIGH);
            }
          } else {
            digitalWrite(LED_FAIL,LOW);delay(200);digitalWrite(LED_FAIL,HIGH);
          }
        }
        else if(mid==0x20){ // delete
          if(res==0){digitalWrite(LED_OK,LOW);delay(200);digitalWrite(LED_OK,HIGH);}
          else       {digitalWrite(LED_FAIL,LOW);delay(200);digitalWrite(LED_FAIL,HIGH);}
        }
        else if(mid==0x1D){ // enroll
          if(res==0&&blen>=4){
            uint16_t uid=(bodyBuf[2]<<8)|bodyBuf[3];
            Serial.printf("Enroll OK UID=%u Name=%s\n",uid,lastEnrollName.c_str());
            digitalWrite(LED_OK,LOW);delay(200);digitalWrite(LED_OK,HIGH);
          } else {
            digitalWrite(LED_FAIL,LOW);delay(200);digitalWrite(LED_FAIL,HIGH);
          }
        }
        ps=S1;
      } break;
    }
  }
}
