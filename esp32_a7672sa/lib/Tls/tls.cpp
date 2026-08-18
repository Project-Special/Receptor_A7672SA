#include "tls.h"

// O AT+CCHSEND aceita no máximo 2048 bytes por comando (+CCHSEND: (0,1),(1-2048)).
static const size_t kSendChunk = 1024;
static const int    kChannel   = 0;

bool A7672Tls::begin() {
  if (_started) return true;

  // Mesmo contexto SSL do cliente HTTP — e com o mesmo cuidado com SNI, sem o
  // qual o handshake morre contra qualquer host atrás de CDN.
  _c.at("AT+CSSLCFG=\"sslversion\",0,4");
  _c.at("AT+CSSLCFG=\"authmode\",0,0");
  _c.at("AT+CSSLCFG=\"enableSNI\",0,1");

  // Modo push: o módulo entrega os dados sozinho, anunciando cada bloco com
  // '+CCHRECV: DATA,<id>,<n>' seguido de exatamente n bytes.
  _c.at("AT+CCHSET=0,0");

  if (!_c.at("AT+CCHSTART", 20000)) {
    // Já iniciado numa sessão anterior responde ERROR — só é falha de verdade
    // se o CCHSTOP seguinte também não funcionar.
    _c.at("AT+CCHSTOP", 10000);
    if (!_c.at("AT+CCHSTART", 20000)) return false;
  }
  _c.waitToken("+CCHSTART:", 20000);

  _c.at("AT+CCHSSLCFG=" + String(kChannel) + ",0");
  _started = true;
  return true;
}

void A7672Tls::end() {
  if (!_started) return;
  close();
  _c.at("AT+CCHSTOP", 10000);
  _started = false;
}

bool A7672Tls::open(const String& host, uint16_t port) {
  // type 2 = SSL/TLS.
  _c.write("AT+CCHOPEN=" + String(kChannel) + ",\"" + host + "\"," + String(port) + ",2\r\n");

  // '+CCHOPEN: <id>,<err>' com err 0 é sucesso. O handshake leva alguns segundos.
  String line;
  uint32_t deadline = millis() + _timeout;
  while ((int32_t)(deadline - millis()) > 0) {
    if (!_c.readLine(line, 200)) continue;
    if (line.startsWith("+CCHOPEN:")) {
      int comma = line.lastIndexOf(',');
      return comma > 0 && line.substring(comma + 1).toInt() == 0;
    }
    if (line == "ERROR") return false;
  }
  return false;
}

void A7672Tls::close() {
  _c.at("AT+CCHCLOSE=" + String(kChannel), 8000);
}

bool A7672Tls::sendAll(const String& data) {
  size_t offset = 0;
  while (offset < data.length()) {
    size_t n = data.length() - offset;
    if (n > kSendChunk) n = kSendChunk;

    _c.write("AT+CCHSEND=" + String(kChannel) + "," + String(n) + "\r\n");
    if (!_c.waitPrompt(8000)) return false;

    _c.write(data.substring(offset, offset + n));
    if (!_c.waitToken("OK", 15000)) return false;
    offset += n;
  }
  return true;
}

// Com 'Connection: close' o fim da resposta é o próprio peer fechando, o que
// evita ter de confiar no Content-Length antes de saber se ele existe.
bool A7672Tls::collect(String& raw) {
  raw = "";
  String line;
  uint32_t deadline = millis() + _timeout;

  while ((int32_t)(deadline - millis()) > 0) {
    if (!_c.readLine(line, 200)) continue;

    if (line.startsWith("+CCH_PEER_CLOSED")) break;
    if (line.startsWith("+CCHCLOSE")) break;

    if (line.startsWith("+CCHRECV: DATA")) {
      // '+CCHRECV: DATA,<id>,<n>' — os próximos n bytes são payload cru e
      // precisam ser lidos por contagem: readLine() comeria CR/LF do corpo.
      int comma = line.lastIndexOf(',');
      if (comma < 0) continue;
      long n = line.substring(comma + 1).toInt();
      if (n <= 0) continue;

      while (n > 0) {
        uint8_t buf[128];
        size_t want = n > (long)sizeof(buf) ? sizeof(buf) : (size_t)n;
        size_t got = _c.readRaw(buf, want, 15000);
        if (!got) return raw.length() > 0;
        if (raw.length() < _maxBody + 2048) raw.concat((const char*)buf, got);
        n -= got;
      }
      deadline = millis() + _timeout;   // houve progresso
    }
  }
  return raw.length() > 0;
}

String A7672Tls::dechunk(const String& in) {
  String out;
  out.reserve(in.length());
  int i = 0;
  while (i < (int)in.length()) {
    int eol = in.indexOf("\r\n", i);
    if (eol < 0) break;
    // O tamanho vem em hexadecimal, podendo trazer extensões após ';'.
    String sizeLine = in.substring(i, eol);
    int semi = sizeLine.indexOf(';');
    if (semi >= 0) sizeLine = sizeLine.substring(0, semi);
    long n = strtol(sizeLine.c_str(), nullptr, 16);
    if (n <= 0) break;
    int start = eol + 2;
    if (start + n > (int)in.length()) n = in.length() - start;
    out += in.substring(start, start + n);
    i = start + n + 2;   // pula o CRLF que fecha o chunk
  }
  return out;
}

bool A7672Tls::parse(const String& raw, TlsResponse& out, size_t maxBody) {
  int head = raw.indexOf("HTTP/1.");
  if (head < 0) return false;

  int sp = raw.indexOf(' ', head);
  if (sp < 0) return false;
  out.status = raw.substring(sp + 1, sp + 5).toInt();

  int sep = raw.indexOf("\r\n\r\n", head);
  if (sep < 0) return false;

  String headers = raw.substring(head, sep);
  headers.toLowerCase();
  String body = raw.substring(sep + 4);

  if (headers.indexOf("transfer-encoding: chunked") >= 0) body = dechunk(body);
  if (body.length() > maxBody) body = body.substring(0, maxBody);

  out.body = body;
  out.ok = (out.status >= 200 && out.status < 300);
  return true;
}

TlsResponse A7672Tls::request(const String& method, const String& host, const String& path,
                              const String& body, const String& contentType,
                              const String& extraHeaders) {
  TlsResponse r;

  if (!_c.requireSim("Requisicao TLS")) return r;
  if (!_n.dataReady()) { r.status = -1; return r; }
  if (!begin()) return r;

  // O peer fecha sockets ociosos em segundos: abrir e só depois montar o
  // request é pedir para tomar +CCH_PEER_CLOSED antes do primeiro byte.
  String req = method + " " + path + " HTTP/1.1\r\n"
               "Host: " + host + "\r\n"
               "User-Agent: A7672SA\r\n"
               "Accept: */*\r\n"
               "Connection: close\r\n";
  if (extraHeaders.length()) req += extraHeaders;
  if (body.length()) {
    if (contentType.length()) req += "Content-Type: " + contentType + "\r\n";
    req += "Content-Length: " + String(body.length()) + "\r\n";
  }
  req += "\r\n";
  req += body;

  if (!open(host, 443)) { close(); return r; }
  if (!sendAll(req))    { close(); return r; }

  String raw;
  if (collect(raw)) parse(raw, r, _maxBody);
  close();
  return r;
}
