#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <time.h>

// ===== Wi‑Fi =====
const char* ssid     = "SSID";
const char* password = "PASS";

// ===== Serial2 =====
#define FACE_SERIAL Serial2
const int RX_PIN    = 16;
const int TX_PIN    = 17;
const uint32_t BAUD = 115200;

// ===== HTTP =====
AsyncWebServer server(80);

// ===== Unlock log =====
struct UnlockEntry { time_t ts; uint16_t uid; String name; };
UnlockEntry unlockLog[100];
int logHead = 0, logCount = 0;

// ===== Hardware button & LEDs =====
const int BUTTON_PIN = 4;    // кнопка разблокировки
const int LED_BUSY   = 5;    // горит во время ожидания ответа
const int LED_OK     = 18;   // вспыхивает при успешной разблокировке
const int LED_FAIL   = 19;   // вспыхивает при ошибке разблокировки

// Дебаунс
bool    lastButtonState = HIGH;
bool    unlocking      = false;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;
String lastEnrollName;
// Текущее ожидаемое событие разблокировки
bool waitingUnlock = false;

// MR_RESULTS...
const char* MR_RESULTS[] = {
  "Операция успешна", "Модуль отклонил команду", "Процесс прерван", nullptr,
  "Не удалось открыть камеру", "Неизвестная ошибка","Неверный параметр",
  "Недостаточно памяти","Пользователь не найден","Превышено число пользователей",
  "Лицо уже зарегистрировано", nullptr,nullptr,
  "Проверка на живое лицо не пройдена","Время ожидания истекло",
  "Неудачная авторизация", nullptr,nullptr,nullptr,
  "Не удалось прочитать файл","Не удалось записать файл",
  "Протокол не зашифрован", nullptr,
  "RGB-изображение не готово", nullptr,
  "Фото слишком большое","Фото слишком маленькое"
};

// ===== SPIFFS =====
void saveUnlockLogFS(){
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  int start = (logHead - logCount + 100) % 100;
  for(int i=0;i<logCount;i++){
    auto &e = unlockLog[(start+i)%100];
    JsonObject o = arr.createNestedObject();
    struct tm tm_info; localtime_r(&e.ts, &tm_info);
    char buf[20]; strftime(buf,sizeof(buf),"%Y-%m-%d %H:%M:%S",&tm_info);
    o["ts"]=buf; o["uid"]=e.uid; o["name"]=e.name;
  }
  File f = SPIFFS.open("/unlocklog.json", FILE_WRITE);
  if(f){ serializeJson(arr,f); f.close(); }
}

void loadUnlockLogFS(){
  if(!SPIFFS.exists("/unlocklog.json")) return;
  File f = SPIFFS.open("/unlocklog.json", FILE_READ);
  if(!f) return;
  DynamicJsonDocument doc(4096);
  auto err = deserializeJson(doc, f);
  f.close();
  if(err == DeserializationError::Ok){
    JsonArray arr = doc.as<JsonArray>();
    logCount = min(100, (int)arr.size());
    logHead  = logCount % 100;
    int idx = 0;
    for(auto o : arr){
      struct tm tm_info;
      strptime(o["ts"] | "", "%Y-%m-%d %H:%M:%S", &tm_info);
      unlockLog[idx].ts   = mktime(&tm_info);
      unlockLog[idx].uid  = o["uid"] | 0;
      unlockLog[idx].name = o["name"].as<String>();
      idx++;
    }
  }
}

// ===== Utils =====
uint8_t getParity(const uint8_t* data, size_t len){
  uint8_t p=0; for(size_t i=2;i<len;i++) p ^= data[i]; return p;
}
void sendPacket(const uint8_t* data, size_t len){
  FACE_SERIAL.write(data,len);
}

// ===== Logging =====
void addUnlockLog(uint16_t uid, const String& name){
  unlockLog[logHead] = { time(nullptr), uid, name };
  logHead = (logHead+1)%100;
  if(logCount<100) ++logCount;
  saveUnlockLogFS();
}

// ===== Packet builders =====
std::vector<uint8_t> makeSimpleCmd(uint8_t id){
  std::vector<uint8_t> p;
  uint16_t sync=0xEFAA,sz=0;
  p.push_back(sync>>8);p.push_back(sync&0xFF);
  p.push_back(id);
  p.push_back(sz>>8); p.push_back(sz&0xFF);
  p.push_back(getParity(p.data(),p.size()));
  return p;
}
std::vector<uint8_t> makeUnlock(){
  std::vector<uint8_t> p;
  uint16_t sync=0xEFAA,sz=2;
  p.push_back(sync>>8);p.push_back(sync&0xFF);
  p.push_back(0x12);
  p.push_back(sz>>8);p.push_back(sz&0xFF);
  p.push_back(0); p.push_back(5);
  p.push_back(getParity(p.data(),p.size()));
  return p;
}
std::vector<uint8_t> makeEnroll(const String& name){
  std::vector<uint8_t> p;
  uint16_t sync=0xEFAA,sz=35;
  p.push_back(sync>>8);p.push_back(sync&0xFF);
  p.push_back(0x1D);
  p.push_back(sz>>8);p.push_back(sz&0xFF);
  p.push_back(0);
  for(int i=0;i<32;i++) p.push_back(i<name.length()?name[i]:0);
  p.push_back(0); p.push_back(0);
  p.push_back(getParity(p.data(),p.size()));
  return p;
}
std::vector<uint8_t> makeDelUser(uint16_t uid){
  std::vector<uint8_t> p;
  uint16_t sync=0xEFAA,sz=2;
  p.push_back(sync>>8);p.push_back(sync&0xFF);
  p.push_back(0x20);
  p.push_back(sz>>8);p.push_back(sz&0xFF);
  p.push_back(uid>>8);p.push_back(uid&0xFF);
  p.push_back(getParity(p.data(),p.size()));
  return p;
}

// ===== Web UI =====
const char indexHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Face Control</title>
  <style>
    body {
      font-family: 'Segoe UI', Tahoma, sans-serif;
      background: #f4f7fa;
      color: #333;
      margin: 0;
      padding: 20px;
    }
    .container {
      max-width: 800px;
      margin: 0 auto;
    }
    h1 {
      text-align: center;
      margin-bottom: 1rem;
    }
    .card {
      background: #fff;
      border-radius: 8px;
      box-shadow: 0 2px 5px rgba(0,0,0,0.1);
      padding: 1.5rem;
      margin-bottom: 1.5rem;
    }
    .card h2 {
      margin-top: 0;
      font-size: 1.2rem;
      color: #005a9c;
    }
    .card .controls {
      display: flex;
      gap: 0.5rem;
      flex-wrap: wrap;
    }
    .card .controls input {
      flex: 1;
      padding: 0.5rem;
      border: 1px solid #ccc;
      border-radius: 4px;
    }
    .card .controls button {
      padding: 0.5rem 1rem;
      border: none;
      background: #0078d7;
      color: white;
      border-radius: 4px;
      cursor: pointer;
      transition: background 0.2s;
    }
    .card .controls button:hover {
      background: #005a9c;
    }
    #status {
      background: #e1f0ff;
      padding: 0.5rem;
      border-radius: 4px;
      min-height: 2rem;
      margin-top: 0.5rem;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      margin-top: 1rem;
    }
    th, td {
      padding: 0.5rem;
      border-bottom: 1px solid #ddd;
      text-align: left;
    }
    th {
      background: #f0f4f8;
    }
    @media (max-width: 600px) {
      .card .controls {
        flex-direction: column;
      }
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>Система распознавания морд</h1>

    <div class="card">
      <h2>Управление пользователями</h2>
      <div class="controls">
        <input id="username" type="text" placeholder="Имя для регистрации">
        <button onclick="enroll()">Зарегистрировать</button>
        <input id="delid" type="text" placeholder="UID для удаления">
        <button onclick="deluser()">Удалить пользователя</button>
      </div>
      <div id="status"></div>
    </div>

    <div class="card">
      <h2>Распознать</h2>
      <div class="controls">
        <button id="uiUnlockBtn" onclick="doUnlock()">Старт</button>
      </div>
      <div id="status"></div>
    </div>

    <div class="card">
      <h2>Журнал регистрации</h2>
      <table id="log">
        <thead>
          <tr><th>Дата/Время</th><th>UID</th><th>Имя</th></tr>
        </thead>
        <tbody></tbody>
      </table>
    </div>
  </div>

<script>
  const statusEls = document.querySelectorAll('#status');
  const logTbody = document.querySelector('#log tbody');

  function setStatus(msg) {
    statusEls.forEach(el => el.textContent = msg);
  }

  function enroll() {
    const name = encodeURIComponent(document.getElementById('username').value);
    fetch('/enroll?username=' + name)
      .then(r => r.text())
      .then(t => setStatus(t));
  }

  function deluser() {
    const id = encodeURIComponent(document.getElementById('delid').value);
    fetch('/deluser?id=' + id)
      .then(r => r.text())
      .then(t => setStatus(t));
  }

  function doUnlock() {
    document.getElementById('uiUnlockBtn').disabled = true;
    fetch('/unlock')
      .then(r => r.text())
      .then(t => setStatus(t))
      .finally(_ => setTimeout(() => {
        document.getElementById('uiUnlockBtn').disabled = false;
      }, 500));
  }

  function loadLog() {
    fetch('/log')
      .then(r => r.json())
      .then(arr => {
        logTbody.innerHTML = '';
        arr.forEach(e => {
          const row = document.createElement('tr');
          row.innerHTML = `<td>${e.ts}</td><td>${e.uid}</td><td>${e.name}</td>`;
          logTbody.appendChild(row);
        });
      });
  }

  setInterval(loadLog, 3000);
  window.addEventListener('load', loadLog);
</script>
</body>
</html>
)rawliteral";

// ===== Parser =====
enum { S1,S2,ST,S3,S4,SB,SC };
int ps=S1, blen=0, bi=0;
uint8_t hbuf[3], bodyBuf[256];

void setup(){
  // Serial & Serial2
  Serial.begin(115200);
  FACE_SERIAL.begin(BAUD, SERIAL_8N1, RX_PIN, TX_PIN);

  // SPIFFS
  if(!SPIFFS.begin(true)) Serial.println("SPIFFS FAIL");
  loadUnlockLogFS();

  // GPIO
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUSY, OUTPUT);
  pinMode(LED_OK,   OUTPUT);
  pinMode(LED_FAIL, OUTPUT);
  digitalWrite(LED_BUSY, LOW);
  digitalWrite(LED_OK,   LOW);
  digitalWrite(LED_FAIL, LOW);

  // Wi‑Fi + NTP
  WiFi.begin(ssid, password);
  while(WiFi.status()!=WL_CONNECTED) delay(300);
 
 
 // Установка часового пояса до configTime
//setenv("TZ", "MSK-3", 1);  // Moscow time (UTC+3)
//tzset();

// Настройка времени через NTP
configTzTime("UTC-3", "pool.ntp.org", "time.nist.gov");

// Ждём синхронизации времени
struct tm timeinfo;
while (!getLocalTime(&timeinfo)) {
  Serial.println("Waiting for time...");
  delay(500);
}

Serial.println("Time synchronized!");
Serial.printf("Current time: %s", asctime(&timeinfo));

  // HTTP
  server.on("/", HTTP_GET,[](AsyncWebServerRequest*r){
    r->send_P(200,"text/html",indexHtml);
  });
  server.on("/unlock", HTTP_GET,[](AsyncWebServerRequest*r){
    // отправляем пакет и включаем BUSY-LED
    auto p = makeUnlock();
    sendPacket(p.data(), p.size());
    digitalWrite(LED_BUSY, HIGH);
    r->send(200,"text/plain","Unlock sent");
  });
  server.on("/log", HTTP_GET,[](AsyncWebServerRequest*r){
    DynamicJsonDocument d(4096);
    JsonArray arr = d.to<JsonArray>();
    int st = (logHead-logCount+100)%100;
    for(int i=0;i<logCount;i++){
      auto &e=unlockLog[(st+i)%100];
      JsonObject o=arr.createNestedObject();
      struct tm tm_info; localtime_r(&e.ts,&tm_info);
      char buf[20]; strftime(buf,sizeof(buf),"%Y-%m-%d %H:%M:%S",&tm_info);
      o["ts"]=buf; o["uid"]=e.uid; o["name"]=e.name;
    }
    String out; serializeJson(arr,out);
    r->send(200,"application/json",out);
  });
  server.on("/enroll", HTTP_GET,[](AsyncWebServerRequest*r){
    if(r->hasParam("username")){
      String n=r->getParam("username")->value();
      lastEnrollName=n;
      auto p=makeEnroll(n); sendPacket(p.data(),p.size());
      r->send(200,"text/plain","Enroll sent");
    }else r->send(400,"text/plain","no name");
  });
  server.on("/deluser", HTTP_GET,[](AsyncWebServerRequest*r){
    if(r->hasParam("id")){
      uint16_t uid=r->getParam("id")->value().toInt();
      auto p=makeDelUser(uid); sendPacket(p.data(),p.size());
      r->send(200,"text/plain","Del user sent");
    }else r->send(400,"text/plain","no id");
  });
  server.on("/delall", HTTP_GET,[](AsyncWebServerRequest*r){
    auto p=makeSimpleCmd(0x21); sendPacket(p.data(),p.size());
    r->send(200,"text/plain","DelAll sent");
  });
  server.on("/getuser", HTTP_GET,[](AsyncWebServerRequest*r){
    auto p=makeSimpleCmd(0x24); sendPacket(p.data(),p.size());
    r->send(200,"text/plain","GetUser sent");
  });
  server.begin();
}

void loop(){
  // Обработка кнопки
  int reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }
  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading == LOW && !unlocking) {
      // нажали кнопку
      unlocking = true;
      // симулируем HTTP-запрос
      auto p = makeUnlock();
      sendPacket(p.data(), p.size());
      digitalWrite(LED_BUSY, HIGH);
      // блокируем кнопку UI на время
      unlocking = false;
    }
  }
  lastButtonState = reading;

  // Чтение ответов от модуля
  while(FACE_SERIAL.available()){
    uint8_t b = FACE_SERIAL.read();
    switch(ps){
      case S1: if(b==0xEF) ps=S2; break;
      case S2: if(b==0xAA) ps=ST; else ps=S1; break;
      case ST: hbuf[0]=b; ps=S3; break;
      case S3: hbuf[1]=b; ps=S4; break;
      case S4:
        hbuf[2]=b;
        blen=(hbuf[1]<<8)|hbuf[2];
        bi=0;
        ps = (blen<=sizeof(bodyBuf))? SB:S1;
        break;
      case SB:
        bodyBuf[bi++]=b;
        if(bi>=blen) ps=SC;
        break;
      case SC:{
        uint8_t mt=hbuf[0], mid=bodyBuf[0], res=bodyBuf[1];
        if(mt==0x00 && mid==0x12){
          // отключаем BUSY
          digitalWrite(LED_BUSY, LOW);
          if(res==0 && blen>=4){
            uint16_t uid=(bodyBuf[2]<<8)|bodyBuf[3];
            int nl=min(blen-4,32);
            char nm[33]; memcpy(nm,&bodyBuf[4],nl); nm[nl]=0;
            String name=String(nm); name.trim();
            addUnlockLog(uid,name);
            Serial.printf("Unlock OK: UID=%u Name=%s\n", uid, name.c_str());
            // success LED
            digitalWrite(LED_OK, HIGH);
            delay(200);
            digitalWrite(LED_OK, LOW);
          }
          else {
            const char* msg = (res<sizeof(MR_RESULTS)/sizeof(MR_RESULTS[0])&&MR_RESULTS[res])?MR_RESULTS[res]:"Unknown";
            Serial.printf("Unlock FAIL: %s\n", msg);
            digitalWrite(LED_FAIL, HIGH);
            delay(200);
            digitalWrite(LED_FAIL, LOW);
          }
        }
        ps=S1;
      } break;
    }
  }
}
