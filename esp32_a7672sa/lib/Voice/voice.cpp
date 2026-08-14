#include "voice.h"

bool A7672Voice::begin() {
  if (!_c.requireSim("Servico de voz")) return false;

  if (!_urcHooked) {
    _urcHooked = true;
    _c.addUrcHandler([this](const String& line) { handleUrc(line); });
  }
  return _c.at("AT+CLIP=1");
}

void A7672Voice::setState(CallState s) {
  if (_state == s) return;
  _state = s;

  if (s == CallState::Active && _startedAt == 0) _startedAt = millis();
  if (s == CallState::Idle || s == CallState::Busy || s == CallState::NoAnswer) {
    _startedAt = 0;
    if (s != CallState::Idle) _number = "";
  }
  if (_cb) _cb(_state, _number);
}

uint32_t A7672Voice::durationSec() const {
  return _startedAt ? (millis() - _startedAt) / 1000 : 0;
}

void A7672Voice::handleUrc(const String& line) {
  if (line == "RING") { setState(CallState::Ringing); return; }

  if (line.startsWith("+CLIP:")) {
    int q1 = line.indexOf('"');
    int q2 = line.indexOf('"', q1 + 1);
    if (q1 >= 0 && q2 > q1) _number = line.substring(q1 + 1, q2);
    setState(CallState::Ringing);
    if (_cb) _cb(_state, _number);
    return;
  }

  if (line.startsWith("+CLCC:")) {
    // +CLCC: <id>,<dir>,<stat>,<mode>,<mpty>,"<number>",<type>
    int c1 = line.indexOf(',');
    int c2 = line.indexOf(',', c1 + 1);
    int c3 = line.indexOf(',', c2 + 1);
    if (c2 < 0 || c3 < 0) return;
    int stat = line.substring(c2 + 1, c3).toInt();

    int q1 = line.indexOf('"');
    int q2 = line.indexOf('"', q1 + 1);
    if (q1 >= 0 && q2 > q1) _number = line.substring(q1 + 1, q2);

    switch (stat) {
      case 0: setState(CallState::Active);  break;   // ativa
      case 2: setState(CallState::Dialing); break;   // discando
      case 3: setState(CallState::Dialing); break;   // alerta no outro lado
      case 4: setState(CallState::Ringing); break;   // entrando
      default: break;
    }
    return;
  }

  if (line == "NO CARRIER" || line == "NO ANSWER") setState(CallState::Idle);
  else if (line == "BUSY")                         setState(CallState::Busy);
}

bool A7672Voice::dial(const String& number) {
  if (!_c.requireSim("Chamada de voz")) return false;
  _number = number;
  setState(CallState::Dialing);

  // Sem o ';' o módulo interpreta como chamada de dados e a ligação falha.
  bool ok = _c.at("ATD" + number + ";", 20000);
  if (!ok) setState(CallState::Idle);
  return ok;
}

bool A7672Voice::answer() {
  if (!_c.at("ATA", 15000)) return false;
  setState(CallState::Active);
  return true;
}

bool A7672Voice::hangup() {
  bool ok = _c.at("AT+CHUP", 10000);
  setState(CallState::Idle);
  return ok;
}

bool A7672Voice::setVolume(int level) {
  if (level < 0) level = 0;
  if (level > 5) level = 5;
  return _c.at("AT+CLVL=" + String(level));
}

bool A7672Voice::setAudioDevice(int device) {
  return _c.at("AT+CSDVC=" + String(device));
}

bool A7672Voice::sendDtmf(const String& digits) {
  for (size_t i = 0; i < digits.length(); i++) {
    if (!_c.at(String("AT+VTS=\"") + digits[i] + "\"", 5000)) return false;
    delay(120);
  }
  return true;
}
