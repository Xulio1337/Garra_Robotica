#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESP32Servo.h>
#include "index_html.h"

// ---------- Rede (o ESP32 cria o Wi-Fi) ----------
const char* AP_SSID     = "EspGarraRobotica";
const char* AP_PASSWORD = "novatech13";

// ---------- Configuração dos servos ----------
// minAngle/maxAngle: limites do slider
// fullRange: curso total do servo em graus (pulso 500-2400us = 0 até fullRange)
// home: posição usada só se não houver nenhuma posição salva
// maxSpeed: velocidade máxima em graus/segundo (controle manual)
struct ServoCfg {
  const char* name;
  int pin;
  float minAngle;
  float maxAngle;
  float fullRange;
  float home;
  float maxSpeed;
};

const int NUM_SERVOS = 4;
const ServoCfg CFG[NUM_SERVOS] = {
  {"Garra",                              13, 0, 180, 180, 90, 90},
  {"Base",                               12, 0, 180, 180, 90, 60},
  {"Lateral Direito",                    14, 0,  90, 180, 90, 45},  // servo 180°, limitado a 90°
  {"Lateral Esquerdo (elbow rotation)",  27, 0,  90,  90, 90, 45},  // servo de 90° reais
};

const int PULSE_MIN_US = 500;
const int PULSE_MAX_US = 2400;
const unsigned long TICK_MS = 20;
const unsigned long SAVE_INTERVAL_MS = 1000;

Servo servos[NUM_SERVOS];
float currentAngle[NUM_SERVOS];  // posição estimada atual
float targetAngle[NUM_SERVOS];   // destino (controle manual)
bool  attached[NUM_SERVOS] = {false, false, false, false};

// ---------- Sequência de passos ----------
struct Step {
  float a[NUM_SERVOS];   // pose (ângulo de cada servo)
  uint32_t moveMs;       // tempo para chegar na pose
  uint32_t holdMs;       // pausa depois de chegar
};

const int MAX_STEPS = 30;
Step steps[MAX_STEPS];
int stepCount = 0;

bool playing = false;
bool loopSeq = false;
bool moving = true;      // true = movendo, false = em pausa
int stepIdx = 0;
unsigned long phaseStart = 0;
float startPose[NUM_SERVOS];

Preferences prefs;
WebServer server(80);
unsigned long lastTick = 0;
unsigned long lastSave = 0;
bool unsaved = false;

// ---------- Utilidades ----------
int angleToUs(int i, float angle) {
  return PULSE_MIN_US + (int)((angle / CFG[i].fullRange) * (PULSE_MAX_US - PULSE_MIN_US));
}

void writeServo(int i) {
  servos[i].writeMicroseconds(angleToUs(i, currentAngle[i]));
}

void savePositions() {
  prefs.putBytes("pos", currentAngle, sizeof(currentAngle));
}

void loadPositions() {
  float saved[NUM_SERVOS];
  bool ok = prefs.getBytesLength("pos") == sizeof(saved);
  if (ok) prefs.getBytes("pos", saved, sizeof(saved));

  for (int i = 0; i < NUM_SERVOS; i++) {
    float v = (ok && !isnan(saved[i])) ? saved[i] : CFG[i].home;
    currentAngle[i] = constrain(v, CFG[i].minAngle, CFG[i].maxAngle);
    targetAngle[i]  = currentAngle[i];
    Serial.printf("%s: posicao estimada %.1f graus%s\n",
                  CFG[i].name, currentAngle[i], ok ? "" : " (padrao)");
  }
}

void saveSequence() {
  prefs.putUChar("seqc", (uint8_t)stepCount);
  if (stepCount > 0) prefs.putBytes("seq", steps, stepCount * sizeof(Step));
  else prefs.remove("seq");
}

void loadSequence() {
  int c = prefs.getUChar("seqc", 0);
  if (c > 0 && c <= MAX_STEPS &&
      prefs.getBytesLength("seq") == c * sizeof(Step)) {
    prefs.getBytes("seq", steps, c * sizeof(Step));
    stepCount = c;
  } else {
    stepCount = 0;
  }
  Serial.printf("Sequencia carregada: %d passos\n", stepCount);
}

// Só começa a enviar sinal PWM quando o servo for usado,
// já começando na posição estimada. Assim nada se mexe no boot.
void attachServo(int i) {
  if (attached[i]) return;
  servos[i].setPeriodHertz(50);
  servos[i].attach(CFG[i].pin, PULSE_MIN_US, PULSE_MAX_US);
  writeServo(i);
  attached[i] = true;
}

// ---------- Execução da sequência ----------
void beginMove() {
  moving = true;
  phaseStart = millis();
  for (int i = 0; i < NUM_SERVOS; i++) startPose[i] = currentAngle[i];
}

bool startPlayback(bool loop) {
  if (stepCount == 0) return false;
  for (int i = 0; i < NUM_SERVOS; i++) attachServo(i);
  loopSeq = loop;
  stepIdx = 0;
  playing = true;
  beginMove();
  return true;
}

void stopPlayback() {
  if (!playing) return;
  playing = false;
  for (int i = 0; i < NUM_SERVOS; i++) targetAngle[i] = currentAngle[i];
}

// Retorna true se algum servo se moveu
bool updatePlayback() {
  unsigned long now = millis();
  const Step& s = steps[stepIdx];

  if (moving) {
    float t = s.moveMs ? (float)(now - phaseStart) / s.moveMs : 1.0f;
    if (t > 1.0f) t = 1.0f;
    float e = t * t * (3.0f - 2.0f * t);   // suaviza início e fim
    for (int i = 0; i < NUM_SERVOS; i++) {
      currentAngle[i] = startPose[i] + (s.a[i] - startPose[i]) * e;
      writeServo(i);
    }
    if (t >= 1.0f) {
      moving = false;
      phaseStart = now;
    }
    return true;
  }

  if (now - phaseStart >= s.holdMs) {
    stepIdx++;
    if (stepIdx >= stepCount) {
      if (loopSeq) {
        stepIdx = 0;
      } else {
        stopPlayback();
        return false;
      }
    }
    beginMove();
  }
  return false;
}

// ---------- Controle manual ----------
void setTarget(int i, float angle) {
  angle = constrain(angle, CFG[i].minAngle, CFG[i].maxAngle);
  attachServo(i);
  targetAngle[i] = angle;
}

// Move cada servo um pouco por ciclo (transição suave com desaceleração no final)
bool updateManual() {
  bool moved = false;

  for (int i = 0; i < NUM_SERVOS; i++) {
    if (!attached[i]) continue;

    float diff = targetAngle[i] - currentAngle[i];
    if (fabsf(diff) < 0.001f) continue;

    float maxStep = CFG[i].maxSpeed * (TICK_MS / 1000.0f);
    float step = constrain(diff * 0.10f, -maxStep, maxStep);

    const float minStep = 0.2f;
    if (fabsf(step) < minStep) step = (diff > 0 ? 1 : -1) * min(minStep, fabsf(diff));

    currentAngle[i] += step;
    writeServo(i);
    moved = true;
  }

  return moved;
}

void updateServos() {
  bool moved = playing ? updatePlayback() : updateManual();
  if (moved) unsaved = true;

  // Salva enquanto move (limitado) e imediatamente quando para
  unsigned long now = millis();
  if (unsaved && (!moved || now - lastSave >= SAVE_INTERVAL_MS)) {
    savePositions();
    unsaved = false;
    lastSave = now;
  }
}

// ---------- Handlers HTTP ----------
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleServo() {
  if (!server.hasArg("id") || !server.hasArg("angle")) {
    server.send(400, "text/plain", "Parametros invalidos");
    return;
  }
  int id = server.arg("id").toInt();
  if (id < 0 || id >= NUM_SERVOS) {
    server.send(400, "text/plain", "Servo invalido");
    return;
  }
  stopPlayback();   // mexer manualmente interrompe a sequência
  setTarget(id, server.arg("angle").toFloat());
  server.send(200, "text/plain", "OK");
}

void handleConfig() {
  String json = "[";
  for (int i = 0; i < NUM_SERVOS; i++) {
    json += "{\"name\":\"" + String(CFG[i].name) + "\"";
    json += ",\"min\":" + String((int)CFG[i].minAngle);
    json += ",\"max\":" + String((int)CFG[i].maxAngle);
    json += ",\"home\":" + String((int)CFG[i].home) + "}";
    if (i < NUM_SERVOS - 1) json += ",";
  }
  json += "]";
  server.send(200, "application/json", json);
}

String posJson() {
  String json = "[";
  for (int i = 0; i < NUM_SERVOS; i++) {
    json += String((int)roundf(currentAngle[i]));
    if (i < NUM_SERVOS - 1) json += ",";
  }
  json += "]";
  return json;
}

void handleState() {
  server.send(200, "application/json", posJson());
}

void handleStatus() {
  String json = "{\"playing\":" + String(playing ? 1 : 0);
  json += ",\"step\":" + String(stepIdx);
  json += ",\"pos\":" + posJson() + "}";
  server.send(200, "application/json", json);
}

// Formato: uma linha por passo -> "a0,a1,a2,a3,moveMs,holdMs"
void handleSeqGet() {
  String out;
  for (int n = 0; n < stepCount; n++) {
    for (int i = 0; i < NUM_SERVOS; i++) out += String((int)roundf(steps[n].a[i])) + ",";
    out += String(steps[n].moveMs) + "," + String(steps[n].holdMs) + "\n";
  }
  server.send(200, "text/plain", out);
}

void handleSeqPost() {
  String body = server.arg("plain");
  stopPlayback();

  int count = 0;
  int pos = 0;
  while (pos < (int)body.length() && count < MAX_STEPS) {
    int end = body.indexOf('\n', pos);
    if (end < 0) end = body.length();
    String line = body.substring(pos, end);
    pos = end + 1;
    line.trim();
    if (line.length() == 0) continue;

    float a[NUM_SERVOS];
    unsigned long mv, hd;
    if (sscanf(line.c_str(), "%f,%f,%f,%f,%lu,%lu",
               &a[0], &a[1], &a[2], &a[3], &mv, &hd) != 6) continue;

    for (int i = 0; i < NUM_SERVOS; i++)
      steps[count].a[i] = constrain(a[i], CFG[i].minAngle, CFG[i].maxAngle);
    steps[count].moveMs = constrain(mv, 100UL, 120000UL);
    steps[count].holdMs = constrain(hd, 0UL, 600000UL);
    count++;
  }

  stepCount = count;
  saveSequence();
  server.send(200, "text/plain", String(stepCount));
}

void handlePlay() {
  bool loop = server.arg("loop") == "1";
  if (!startPlayback(loop)) {
    server.send(400, "text/plain", "Sem passos");
    return;
  }
  server.send(200, "text/plain", "OK");
}

void handleStop() {
  stopPlayback();
  server.send(200, "text/plain", "OK");
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  delay(300);

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  prefs.begin("garra", false);
  loadPositions();   // NÃO move nenhum servo
  loadSequence();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("Access Point ativo. IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/servo", handleServo);
  server.on("/config", handleConfig);
  server.on("/state", handleState);
  server.on("/status", handleStatus);
  server.on("/seq", HTTP_GET, handleSeqGet);
  server.on("/seq", HTTP_POST, handleSeqPost);
  server.on("/play", handlePlay);
  server.on("/stop", handleStop);
  server.begin();
  Serial.println("Servidor HTTP iniciado.");
}

void loop() {
  server.handleClient();

  unsigned long now = millis();
  if (now - lastTick >= TICK_MS) {
    lastTick = now;
    updateServos();
  }
}