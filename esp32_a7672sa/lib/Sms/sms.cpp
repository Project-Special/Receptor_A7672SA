#include "sms.h"

// Caracteres da tabela de extensão GSM 03.38: ocupam dois septetos.
static bool isGsmExtended(char c) {
  return c == '{' || c == '}' || c == '[' || c == ']' || c == '~'
      || c == '\\' || c == '|' || c == '^';
}

size_t A7672Sms::gsmSeptets(const String& text) {
  size_t n = 0;
  for (size_t i = 0; i < text.length(); i++) n += isGsmExtended(text[i]) ? 2 : 1;
  return n;
}

size_t A7672Sms::partsFor(const String& text, bool ucs2) {
  size_t len   = ucs2 ? text.length() : gsmSeptets(text);
  size_t single = ucs2 ? 70 : 160;
  size_t multi  = ucs2 ? 67 : 153;   // 6 unidades vão para o cabeçalho UDH
  if (len <= single) return 1;
  return (len + multi - 1) / multi;
}

// UTF-8 -> hex UTF-16BE. Fora do BMP vira par surrogate.
String A7672Sms::toUcs2Hex(const String& s) {
  String out;
  out.reserve(s.length() * 4);
  size_t i = 0;
  const uint8_t* p = (const uint8_t*)s.c_str();
  size_t n = s.length();

  auto emit = [&out](uint16_t v) {
    char b[5];
    snprintf(b, sizeof(b), "%04X", v);
    out += b;
  };

  while (i < n) {
    uint32_t cp; uint8_t c = p[i];
    if (c < 0x80)                    { cp = c; i += 1; }
    else if ((c & 0xE0) == 0xC0 && i + 1 < n) { cp = ((c & 0x1F) << 6) | (p[i+1] & 0x3F); i += 2; }
    else if ((c & 0xF0) == 0xE0 && i + 2 < n) { cp = ((c & 0x0F) << 12) | ((p[i+1] & 0x3F) << 6) | (p[i+2] & 0x3F); i += 3; }
    else if ((c & 0xF8) == 0xF0 && i + 3 < n) { cp = ((c & 0x07) << 18) | ((p[i+1] & 0x3F) << 12) | ((p[i+2] & 0x3F) << 6) | (p[i+3] & 0x3F); i += 4; }
    else                             { cp = '?'; i += 1; }

    if (cp > 0xFFFF) {
      cp -= 0x10000;
      emit(0xD800 + (cp >> 10));
      emit(0xDC00 + (cp & 0x3FF));
    } else emit((uint16_t)cp);
  }
  return out;
}

bool A7672Sms::begin() {
  if (!_c.requireSim("Configuracao de SMS")) return false;
  if (!_c.at("AT+CMGF=1")) return false;              // modo texto
  if (!_c.at("AT+CSCS=\"GSM\"")) return false;
  _c.at("AT+CPMS=\"SM\",\"SM\",\"SM\"");              // memória do SIM
  return _c.at("AT+CNMI=2,1,0,0,0");                  // URC +CMTI ao chegar
}

bool A7672Sms::send(const String& number, const String& text, bool ucs2) {
  if (!_c.requireSim("Envio de SMS")) return false;
  if (!_c.at("AT+CMGF=1")) return false;
  if (!_c.at(String("AT+CSCS=\"") + (ucs2 ? "UCS2" : "GSM") + "\"")) return false;

  String dest = ucs2 ? toUcs2Hex(number) : number;
  String body = ucs2 ? toUcs2Hex(text)   : text;

  _c.write("AT+CMGS=\"" + dest + "\"\r\n");
  if (!_c.waitPrompt(8000)) {
    _c.write("\x1B");                                 // ESC aborta a composição
    return false;
  }
  delay(120);
  _c.write(body);
  _c.write("\x1A");                                   // Ctrl+Z envia

  // Envio real leva bem mais que um comando comum.
  bool ok = _c.waitToken("+CMGS:", 60000);
  if (ok) _c.waitToken("OK", 5000);

  // O charset é global no módulo: deixar em UCS2 quebraria leituras seguintes.
  if (ucs2) _c.at("AT+CSCS=\"GSM\"");
  return ok;
}

bool A7672Sms::read(int index, SmsMessage& out) {
  if (!_c.requireSim("Leitura de SMS")) return false;
  _c.at("AT+CMGF=1");

  _c.write("AT+CMGR=" + String(index) + "\r\n");

  String line;
  bool header = false;
  out.index = index;
  out.text = "";

  uint32_t deadline = millis() + 8000;
  while ((int32_t)(deadline - millis()) > 0) {
    if (!_c.readLine(line, 150)) continue;
    if (line == "OK") return header;
    if (line.startsWith("+CME ERROR") || line.startsWith("ERROR")) return false;

    if (line.startsWith("+CMGR:")) {
      header = true;
      // +CMGR: "REC UNREAD","+5551999...",,"26/08/14,17:20:00-12"
      int a = line.indexOf('"');
      int b = line.indexOf('"', a + 1);
      int c = line.indexOf('"', b + 1);
      int d = line.indexOf('"', c + 1);
      int e = line.indexOf('"', d + 1);
      int f = line.indexOf('"', e + 1);
      if (a >= 0 && b > a) out.status = line.substring(a + 1, b);
      if (c >= 0 && d > c) out.sender = line.substring(c + 1, d);
      if (e >= 0 && f > e) out.timestamp = line.substring(e + 1, f);
      continue;
    }
    if (header) {
      if (out.text.length()) out.text += "\n";
      out.text += line;
    }
  }
  return false;
}

bool A7672Sms::deleteAll()          { return _c.at("AT+CMGD=,4", 15000); }
bool A7672Sms::deleteOne(int index) { return _c.at("AT+CMGD=" + String(index), 10000); }

void A7672Sms::onArrived(SmsArrivedHandler h) {
  _cb = h;
  if (_urcHooked) return;
  _urcHooked = true;

  _c.addUrcHandler([this](const String& line) {
    if (!_cb || !line.startsWith("+CMTI:")) return;
    // +CMTI: "SM",3
    int q1 = line.indexOf('"');
    int q2 = line.indexOf('"', q1 + 1);
    int comma = line.lastIndexOf(',');
    if (q1 < 0 || q2 < 0 || comma < 0) return;
    _cb(line.substring(comma + 1).toInt(), line.substring(q1 + 1, q2));
  });
}
