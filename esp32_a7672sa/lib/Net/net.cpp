#include "net.h"

static String csvField(const String& s, int idx) {
  int start = 0, field = 0;
  for (int i = 0; i <= s.length(); i++) {
    if (i == s.length() || s[i] == ',') {
      if (field == idx) { String v = s.substring(start, i); v.trim(); return v; }
      field++; start = i + 1;
    }
  }
  return "";
}

int A7672Net::rssiToDbm(int rssi) {
  if (rssi < 0 || rssi > 31) return 0;      // 99 = desconhecido
  return -113 + 2 * rssi;
}

bool A7672Net::refresh(NetStatus& out) {
  String r = _c.queryLine("AT+CSQ", "+CSQ:", 5000);
  if (r.length()) {
    String body = r.substring(r.indexOf(':') + 1);
    out.rssi = csvField(body, 0).toInt();
    out.dbm  = rssiToDbm(out.rssi);
  }

  r = _c.queryLine("AT+CESQ", "+CESQ:", 5000);
  if (r.length()) {
    String body = r.substring(r.indexOf(':') + 1);
    int rsrq = csvField(body, 4).toInt();
    int rsrp = csvField(body, 5).toInt();
    // Escalas do 3GPP 27.007: RSRQ = -20 + n/2 dB, RSRP = -141 + n dBm.
    if (rsrq != 255) out.rsrq = -20 + rsrq / 2;
    if (rsrp != 255) out.rsrp = -141 + rsrp;
  }

  r = _c.queryLine("AT+CPSI?", "+CPSI:", 5000);
  if (r.length()) {
    String body = r.substring(r.indexOf(':') + 1);
    out.tech = csvField(body, 0);
  }

  r = _c.queryLine("AT+CEREG?", "+CEREG:", 5000);
  if (r.length()) {
    String body = r.substring(r.indexOf(':') + 1);
    out.cereg = csvField(body, 1).toInt();
  }

  r = _c.queryLine("AT+COPS?", "+COPS:", 8000);
  if (r.length()) {
    int q1 = r.indexOf('"');
    int q2 = r.indexOf('"', q1 + 1);
    if (q1 >= 0 && q2 > q1) out.operatorName = r.substring(q1 + 1, q2);
  }

  r = _c.queryLine("AT+CGATT?", "+CGATT:", 5000);
  out.attached = r.length() && r.indexOf("1") > r.indexOf(':');

  r = _c.queryLine("AT+CGPADDR", "+CGPADDR:", 5000);
  if (r.length()) {
    int q1 = r.indexOf('"');
    if (q1 >= 0) {
      int q2 = r.indexOf('"', q1 + 1);
      out.ip = r.substring(q1 + 1, q2);
    } else {
      out.ip = csvField(r.substring(r.indexOf(':') + 1), 1);
    }
  }
  return true;
}

int A7672Net::signalDbm() {
  String r = _c.queryLine("AT+CSQ", "+CSQ:", 5000);
  if (!r.length()) return 0;
  return rssiToDbm(csvField(r.substring(r.indexOf(':') + 1), 0).toInt());
}

bool A7672Net::registered() {
  String r = _c.queryLine("AT+CEREG?", "+CEREG:", 5000);
  if (!r.length()) return false;
  int st = csvField(r.substring(r.indexOf(':') + 1), 1).toInt();
  return st == 1 || st == 5;
}

bool A7672Net::setApn(const String& apn, const String& user, const String& pass) {
  if (!_c.requireSim("Configuracao de APN")) return false;
  if (!_c.at("AT+CGDCONT=1,\"IP\",\"" + apn + "\"")) return false;
  if (user.length() || pass.length()) {
    if (!_c.at("AT+CGAUTH=1,1,\"" + user + "\",\"" + pass + "\"")) return false;
  }
  return true;
}

bool A7672Net::attach(uint32_t timeoutMs) {
  if (!_c.requireSim("Attach de pacotes")) return false;
  return _c.at("AT+CGATT=1", timeoutMs);
}

bool A7672Net::waitRegistered(uint32_t timeoutMs) {
  if (!_c.requireSim("Registro na rede")) return false;
  uint32_t deadline = millis() + timeoutMs;
  while ((int32_t)(deadline - millis()) > 0) {
    if (registered()) return true;
    delay(2000);
  }
  return false;
}

bool A7672Net::dataReady(String* ipOut) {
  String r = _c.queryLine("AT+CGATT?", "+CGATT:", 5000);
  if (!r.length() || r.indexOf("1") < r.indexOf(':')) return false;

  r = _c.queryLine("AT+CGPADDR", "+CGPADDR:", 5000);
  if (!r.length()) return false;

  // Contexto pode existir sem endereço atribuído.
  if (r.indexOf("0.0.0.0") >= 0) return false;

  if (ipOut) {
    int q1 = r.indexOf('"');
    *ipOut = (q1 >= 0) ? r.substring(q1 + 1, r.indexOf('"', q1 + 1))
                       : csvField(r.substring(r.indexOf(':') + 1), 1);
  }
  return true;
}
