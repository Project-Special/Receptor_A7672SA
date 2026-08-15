#include "core.h"

bool A7672Core::begin(HardwareSerial& port, int rxPin, int txPin, uint32_t baud) {
  _ser = &port;
  _ser->begin(baud, SERIAL_8N1, rxPin, txPin);
  _ser->setRxBufferSize(2048);   // respostas de HTTPREAD chegam em rajada
  _buf.reserve(256);
  return true;
}

void A7672Core::powerOn(int pwrkeyPin, bool activeHigh, uint32_t pulseMs) {
  pinMode(pwrkeyPin, OUTPUT);
  digitalWrite(pwrkeyPin, activeHigh ? LOW : HIGH);
  delay(100);
  digitalWrite(pwrkeyPin, activeHigh ? HIGH : LOW);
  delay(pulseMs);
  digitalWrite(pwrkeyPin, activeHigh ? LOW : HIGH);
}

void A7672Core::setStatusPin(int pin, bool activeHigh) {
  _statusPin = pin;
  _statusActiveHigh = activeHigh;
  if (pin >= 0) pinMode(pin, INPUT);
}

bool A7672Core::isPoweredOn(uint32_t probeMs) {
  // STATUS é a evidência direta: fica alto só após o boot completar e baixo o
  // resto do tempo. Não depende da UART nem consome um comando.
  if (_statusPin >= 0)
    return digitalRead(_statusPin) == (_statusActiveHigh ? HIGH : LOW);

  // Sem o STATUS ligado, a própria resposta ao AT responde por ele.
  return at("AT", probeMs);
}

bool A7672Core::ensurePowered(int pwrkeyPin, bool activeHigh, uint32_t readyTimeoutMs) {
  if (isPoweredOn()) {
    // Pulsar o PWRKEY com o módulo já rodando é inócuo (desligar exige 2,5 s),
    // mas custa os ~8 s de Ton(uart) à toa e reinicia o que já funcionava.
    if (_dbg) _dbg->println(F("[PWR] modulo ja ligado"));
    return waitReady(readyTimeoutMs);
  }

  if (_dbg) _dbg->println(F("[PWR] pulsando PWRKEY"));
  powerOn(pwrkeyPin, activeHigh);
  return waitReady(readyTimeoutMs);   // Ton(uart) do datasheet: ~8 s
}

bool A7672Core::waitReady(uint32_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  while ((int32_t)(deadline - millis()) > 0) {
    if (at("AT", 1200)) return true;
    delay(300);
  }
  _lastError = "modulo nao respondeu a AT";
  return false;
}

void A7672Core::write(const String& s) {
  if (!_ser) return;
  if (_dbg) { _dbg->print(F(">> ")); _dbg->println(s); }
  _ser->print(s);
}

void A7672Core::writeRaw(const uint8_t* data, size_t len) {
  if (_ser) _ser->write(data, len);
}

size_t A7672Core::readRaw(uint8_t* data, size_t len, uint32_t timeoutMs) {
  if (!_ser || !data || !len) return 0;

  size_t received = 0;
  uint32_t deadline = millis() + timeoutMs;
  while (received < len && (int32_t)(deadline - millis()) > 0) {
    while (_ser->available() && received < len)
      data[received++] = (uint8_t)_ser->read();
    if (received < len) delay(1);
  }
  return received;
}

// Lê até '\n'. Descarta linhas vazias. O prompt '>' vem sem quebra de linha,
// então é reconhecido assim que o buffer o contém e nada mais chega.
bool A7672Core::readLine(String& out, uint32_t timeoutMs) {
  if (!_ser) return false;
  uint32_t deadline = millis() + timeoutMs;

  while ((int32_t)(deadline - millis()) > 0) {
    while (_ser->available()) {
      char c = (char)_ser->read();
      if (c == '\n') {
        _buf.trim();
        if (_buf.length()) { out = _buf; _buf = ""; return true; }
        _buf = "";
        continue;
      }
      if (c != '\r') _buf += c;
      if (_buf.length() > 1024) { out = _buf; _buf = ""; return true; }  // linha absurda
    }
    // prompt sem newline
    String t = _buf; t.trim();
    if (t == ">") { out = ">"; _buf = ""; return true; }
    delay(2);
  }
  return false;
}

bool A7672Core::isFinal(const String& l) {
  return l == "OK" || l.startsWith("ERROR")
      || l.startsWith("+CME ERROR") || l.startsWith("+CMS ERROR");
}

void A7672Core::dispatch(const String& line) {
  trackSim(line);
  if (_dbg) { _dbg->print(F("<< ")); _dbg->println(line); }
  for (auto& h : _urc) h(line);
}

void A7672Core::trackSim(const String& line) {
  SimState before = _sim;
  if (line.indexOf("SIM not inserted") >= 0 || line.indexOf("NOT INSERTED") >= 0
      || line.indexOf("+CME ERROR: 10") >= 0)                 _sim = SimState::Absent;
  else if (line.startsWith("+CPIN: READY"))                   _sim = SimState::Ready;
  else if (line.startsWith("+CPIN: SIM PIN")
        || line.startsWith("+CPIN: SIM PUK"))                 _sim = SimState::Locked;
  else return;

  if (_sim != before && _dbg) {
    _dbg->print(F("[SIM] ")); _dbg->println(simText(_sim));
  }
}

bool A7672Core::at(const String& cmd, uint32_t timeoutMs) {
  write(cmd + "\r\n");
  String line;
  uint32_t deadline = millis() + timeoutMs;

  while ((int32_t)(deadline - millis()) > 0) {
    if (!readLine(line, 120)) continue;
    if (line == cmd) continue;                 // eco
    dispatch(line);
    if (isFinal(line)) {
      if (line == "OK") { _lastError = ""; return true; }
      _lastError = line;
      return false;
    }
  }
  _lastError = "timeout em " + cmd;
  return false;
}

String A7672Core::queryLine(const String& cmd, const String& prefix, uint32_t timeoutMs) {
  write(cmd + "\r\n");
  String line, hit = "";
  uint32_t deadline = millis() + timeoutMs;

  while ((int32_t)(deadline - millis()) > 0) {
    if (!readLine(line, 120)) continue;
    if (line == cmd) continue;
    dispatch(line);
    if (hit.length() == 0 && line.startsWith(prefix)) hit = line;
    if (isFinal(line)) { if (line != "OK") _lastError = line; break; }
  }
  return hit;
}

bool A7672Core::waitToken(const String& token, uint32_t timeoutMs) {
  String line;
  uint32_t deadline = millis() + timeoutMs;
  while ((int32_t)(deadline - millis()) > 0) {
    if (!readLine(line, 120)) continue;
    dispatch(line);
    if (line.startsWith(token)) return true;
  }
  return false;
}

bool A7672Core::waitPrompt(uint32_t timeoutMs) {
  String line;
  uint32_t deadline = millis() + timeoutMs;
  while ((int32_t)(deadline - millis()) > 0) {
    if (!readLine(line, 120)) continue;
    if (line == ">") return true;
    dispatch(line);
    if (isFinal(line)) { _lastError = line; return false; }
  }
  _lastError = "sem prompt '>'";
  return false;
}

void A7672Core::pump(uint32_t forMs) {
  String line;
  uint32_t deadline = millis() + forMs;
  do {
    while (readLine(line, 5)) dispatch(line);
  } while ((int32_t)(deadline - millis()) > 0);
}

bool A7672Core::refreshSim() {
  String r = queryLine("AT+CPIN?", "+CPIN:", 5000);
  if (r.length() == 0 && _sim == SimState::Unknown) {
    // Sem SIM o módulo responde '+CME ERROR: SIM not inserted' e nunca '+CPIN:'.
    // O trackSim() já capturou isso pelo dispatch.
  }
  return _sim == SimState::Ready;
}

bool A7672Core::requireSim(const char* what) {
  // Só bloqueia com certeza: em Unknown a primeira consulta precisa passar.
  if (_sim == SimState::Ready || _sim == SimState::Unknown) return true;
  _lastError = String(what) + " cancelado: SIM " + simText(_sim);
  if (_dbg) _dbg->println("[SIM] " + _lastError);
  return false;
}

const char* A7672Core::simText(SimState s) {
  switch (s) {
    case SimState::Ready:  return "pronto";
    case SimState::Absent: return "ausente";
    case SimState::Locked: return "bloqueado (PIN/PUK)";
    default:               return "desconhecido";
  }
}
