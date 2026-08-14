#include "http.h"

HttpResponse A7672Http::get(const String& url) {
  return request(0, url, "", "");
}

HttpResponse A7672Http::post(const String& url, const String& body, const String& contentType) {
  return request(1, url, body, contentType);
}

HttpResponse A7672Http::request(int method, const String& url,
                                const String& body, const String& contentType) {
  HttpResponse r;

  if (!_c.requireSim("Requisicao HTTP")) return r;

  // Sem contexto PDP o AT+HTTPACTION só falha depois de dezenas de segundos,
  // sem dizer que o problema é rede e não HTTP. Verificar antes custa ~1 s.
  if (!_n.dataReady()) {
    r.status = -1;
    return r;
  }

  _c.at("AT+HTTPTERM", 3000);                 // ERROR aqui é normal se não havia sessão
  if (!_c.at("AT+HTTPINIT", 8000)) return r;

  if (!_c.at("AT+HTTPPARA=\"URL\",\"" + url + "\"")) { _c.at("AT+HTTPTERM"); return r; }

  if (url.startsWith("https://")) {
    _c.at("AT+CSSLCFG=\"sslversion\",0,4");
    _c.at("AT+CSSLCFG=\"authmode\",0,0");
    _c.at("AT+HTTPPARA=\"SSLCFG\",0");
  }

  if (method == 1) {
    if (contentType.length()) _c.at("AT+HTTPPARA=\"CONTENT\",\"" + contentType + "\"");
    if (body.length()) {
      _c.write("AT+HTTPDATA=" + String(body.length()) + ",10000\r\n");
      // O A76xx responde 'DOWNLOAD' (não o '>' do CMGS) para pedir o corpo.
      if (!_c.waitToken("DOWNLOAD", 8000)) { _c.at("AT+HTTPTERM"); return r; }
      _c.write(body);                          // exatamente n bytes, sem terminador
      if (!_c.waitToken("OK", 15000))         { _c.at("AT+HTTPTERM"); return r; }
    }
  }

  _c.write("AT+HTTPACTION=" + String(method) + "\r\n");

  // A URC +HTTPACTION pode demorar: é a requisição inteira indo e voltando.
  String line;
  bool got = false;
  uint32_t deadline = millis() + _actionTimeout;
  while ((int32_t)(deadline - millis()) > 0) {
    if (!_c.readLine(line, 200)) continue;
    if (line.startsWith("+HTTPACTION:")) { got = true; break; }
  }
  if (!got) { _c.at("AT+HTTPTERM"); return r; }

  // +HTTPACTION: <method>,<status>,<datalen>
  int c1 = line.indexOf(',');
  int c2 = line.indexOf(',', c1 + 1);
  if (c1 > 0 && c2 > c1) {
    r.status = line.substring(c1 + 1, c2).toInt();
    r.length = (size_t)line.substring(c2 + 1).toInt();
  }

  if (r.length) {
    size_t want = (_maxBody && r.length > _maxBody) ? _maxBody : r.length;
    readBody(want, r.body);
  }

  r.ok = (r.status >= 200 && r.status < 300);
  _c.at("AT+HTTPTERM", 5000);
  return r;
}

// O módulo anuncia o tamanho em '+HTTPREAD: <n>' e envia exatamente n bytes.
// Contamos bytes em vez de procurar um 'OK' terminador: esse 'OK' tanto pode
// fazer parte do payload quanto ser a resposta de outro comando.
bool A7672Http::readBody(size_t total, String& out) {
  out = "";
  out.reserve(total + 16);

  size_t offset = 0;
  while (offset < total) {
    size_t size = total - offset;
    if (size > _chunk) size = _chunk;

    _c.write("AT+HTTPREAD=" + String(offset) + "," + String(size) + "\r\n");

    // Cabeçalho do bloco.
    String line;
    long remaining = -1;
    uint32_t deadline = millis() + 15000;
    while ((int32_t)(deadline - millis()) > 0) {
      if (!_c.readLine(line, 200)) continue;
      if (line.startsWith("+HTTPREAD:")) {
        long n = line.substring(line.indexOf(':') + 1).toInt();
        if (n > 0) { remaining = n; break; }
      }
      if (line.startsWith("+CME ERROR") || line.startsWith("ERROR")) return false;
    }
    if (remaining < 0) return false;

    // Corpo do bloco, por contagem de bytes.
    while (remaining > 0 && (int32_t)(deadline - millis()) > 0) {
      if (!_c.readLine(line, 300)) continue;
      if (line.startsWith("+HTTPREAD:")) break;          // '+HTTPREAD: 0' fecha o bloco
      if (out.length()) out += "\n";
      out += line;
      remaining -= (long)line.length() + 1;              // +1 pelo \n descartado
    }

    offset += size;
    _c.pump(60);   // consome o 'OK' final do bloco
  }
  return true;
}
