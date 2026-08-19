#include "firebase.h"

static const char* kAuthHost    = "identitytoolkit.googleapis.com";
static const char* kRefreshHost = "securetoken.googleapis.com";
static const char* kNvsSpace    = "fbauth";
static const char* kNvsRefresh  = "refresh";

// Extrator mínimo para os campos que interessam: o JSON do Firebase Auth vem
// com ~900 caracteres só de token, e uma lib de JSON completa custaria mais RAM
// do que o ganho — aqui basta achar "chave":"valor".
String A7672Firebase::jsonString(const String& src, const String& key) {
  String needle = "\"" + key + "\"";
  int k = src.indexOf(needle);
  if (k < 0) return "";
  int colon = src.indexOf(':', k + needle.length());
  if (colon < 0) return "";
  int q1 = src.indexOf('"', colon + 1);
  if (q1 < 0) return "";
  int q2 = src.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return src.substring(q1 + 1, q2);
}

String A7672Firebase::isoTimestamp(const String& utcDate, const String& utcTime) {
  // ddmmyy / hhmmss(.s) — sem data o satélite ainda não entregou o quadro certo.
  if (utcDate.length() < 6 || utcTime.length() < 6) return "";

  String dd = utcDate.substring(0, 2);
  String mm = utcDate.substring(2, 4);
  String yy = utcDate.substring(4, 6);
  String hh = utcTime.substring(0, 2);
  String mi = utcTime.substring(2, 4);
  String ss = utcTime.substring(4, 6);

  return "20" + yy + "-" + mm + "-" + dd + "T" + hh + ":" + mi + ":" + ss + "Z";
}

void A7672Firebase::begin(const String& apiKey, const String& dbHost, const String& deviceId) {
  _apiKey = apiKey;
  _dbHost = dbHost;
  _deviceId = deviceId;

  Preferences p;
  if (p.begin(kNvsSpace, true)) {
    _refreshToken = p.getString(kNvsRefresh, "");
    p.end();
  }
}

void A7672Firebase::forgetIdentity() {
  _idToken = "";
  _refreshToken = "";
  _tokenExpiresAt = 0;
  Preferences p;
  if (p.begin(kNvsSpace, false)) {
    p.remove(kNvsRefresh);
    p.end();
  }
}

void A7672Firebase::storeTokens(const String& idToken, const String& refreshToken, long expiresIn) {
  _idToken = idToken;
  if (expiresIn <= 0) expiresIn = 3600;
  _tokenExpiresAt = millis() + (uint32_t)expiresIn * 1000UL;

  if (refreshToken.length() && refreshToken != _refreshToken) {
    _refreshToken = refreshToken;
    Preferences p;
    if (p.begin(kNvsSpace, false)) {
      p.putString(kNvsRefresh, _refreshToken);
      p.end();
    }
  }
}

bool A7672Firebase::signUpAnonymous() {
  TlsResponse r = _t.request("POST", kAuthHost,
                             "/v1/accounts:signUp?key=" + _apiKey,
                             "{\"returnSecureToken\":true}",
                             "application/json");
  if (!r.ok) {
    _lastError = "signUp falhou: HTTP " + String(r.status) + " " + r.body.substring(0, 120);
    return false;
  }

  String id = jsonString(r.body, "idToken");
  if (id.isEmpty()) {
    _lastError = "signUp sem idToken na resposta";
    return false;
  }
  _uid = jsonString(r.body, "localId");
  storeTokens(id, jsonString(r.body, "refreshToken"), jsonString(r.body, "expiresIn").toInt());
  _lastError = "";
  return true;
}

bool A7672Firebase::refreshIdToken() {
  if (_refreshToken.isEmpty()) return false;

  // Este endpoint é form-urlencoded, não JSON, e responde em snake_case.
  TlsResponse r = _t.request("POST", kRefreshHost, "/v1/token?key=" + _apiKey,
                             "grant_type=refresh_token&refresh_token=" + _refreshToken,
                             "application/x-www-form-urlencoded");
  if (!r.ok) {
    _lastError = "refresh falhou: HTTP " + String(r.status) + " " + r.body.substring(0, 120);
    return false;
  }

  String id = jsonString(r.body, "id_token");
  if (id.isEmpty()) return false;

  if (_uid.isEmpty()) _uid = jsonString(r.body, "user_id");
  storeTokens(id, jsonString(r.body, "refresh_token"), jsonString(r.body, "expires_in").toInt());
  _lastError = "";
  return true;
}

bool A7672Firebase::ensureAuth() {
  if (_apiKey.isEmpty() || _dbHost.isEmpty()) {
    _lastError = "Firebase nao configurado (apiKey/dbHost)";
    return false;
  }

  // Renova com folga: token que vence no meio de um envio é erro 401 à toa.
  if (_idToken.length() && (int32_t)(_tokenExpiresAt - millis()) > 5 * 60 * 1000L)
    return true;

  // Reaproveitar a identidade mantém o mesmo UID entre reinícios; só quando o
  // refresh é recusado (revogado, projeto trocado) vale criar outro usuário.
  if (refreshIdToken()) return true;

  return signUpAnonymous();
}

bool A7672Firebase::put(const String& path, const String& json, const String& method) {
  // O token vai na QUERY, não no header. O Realtime Database recusa idToken em
  // 'Authorization: Bearer' com 401 "Unauthorized request." — nesse header ele
  // só aceita access token OAuth2 de conta de serviço. Verificado contra o
  // banco real: header 401, ?auth= 200.
  //
  // Aqui isso é possível porque o request-line é montado por nós sobre o canal
  // SSL cru; o teto de 600 caracteres de URL é do AT+HTTPPARA, que não entra
  // neste caminho (ver lib/Tls/tls.h).
  String url = path + (path.indexOf('?') >= 0 ? "&" : "?") + "auth=" + _idToken;

  TlsResponse r = _t.request(method, _dbHost, url, json, "application/json");
  if (!r.ok) {
    // `path` sem a query: o token de 861 caracteres no log esconderia o erro.
    _lastError = method + " " + path + " -> HTTP " + String(r.status) + " " + r.body.substring(0, 120);
    // 401 costuma ser token vencido: descarta para o próximo ciclo renovar.
    if (r.status == 401) _idToken = "";
    return false;
  }
  return true;
}

// Valor cru de uma chave: serve para números e booleanos, que jsonString() não
// pega por procurar aspas. Devolve "" quando a chave não existe.
String A7672Firebase::jsonRaw(const String& src, const String& key) {
  String needle = "\"" + key + "\"";
  int k = src.indexOf(needle);
  if (k < 0) return "";
  int colon = src.indexOf(':', k + needle.length());
  if (colon < 0) return "";

  int i = colon + 1;
  while (i < (int)src.length() && (src[i] == ' ' || src[i] == '"')) i++;
  int start = i;
  while (i < (int)src.length() && src[i] != ',' && src[i] != '}' && src[i] != '"') i++;

  String v = src.substring(start, i);
  v.trim();
  return v;
}

bool A7672Firebase::fetch(const String& path, String& out) {
  if (!ensureAuth()) return false;

  String url = path + (path.indexOf('?') >= 0 ? "&" : "?") + "auth=" + _idToken;
  TlsResponse r = _t.request("GET", _dbHost, url);
  if (!r.ok) {
    _lastError = "GET " + path + " -> HTTP " + String(r.status) + " " + r.body.substring(0, 120);
    if (r.status == 401) _idToken = "";
    return false;
  }
  out = r.body;
  return true;
}

bool A7672Firebase::fetchConfig() {
  String body;
  if (!fetch("/devices/" + _deviceId + "/config.json", body)) return false;

  // Nó ausente vem como "null": é o estado inicial, antes de o app mandar
  // qualquer ordem. Aí o rastreamento fica desligado, que é o padrão seguro.
  if (body.length() == 0 || body == "null") {
    _enabled = false;
    return true;
  }

  String en = jsonRaw(body, "enabled");
  if (en.length()) _enabled = (en == "true" || en == "1");

  String iv = jsonRaw(body, "interval");
  if (iv.length()) _remoteInterval = (uint32_t)iv.toInt();

  String tk = jsonRaw(body, "track");
  if (tk.length()) _track = (tk == "true" || tk == "1");

  String md = jsonRaw(body, "minDist");
  if (md.length()) _minDist = md.toFloat();

  String se = jsonRaw(body, "statusEvery");
  if (se.length()) _statusEvery = (uint32_t)se.toInt();

  return true;
}

static const char* kNvsSsid = "wifi_ssid";
static const char* kNvsPass = "wifi_pass";

bool A7672Firebase::wifiSalvo(String& ssid, String& pass) {
  Preferences p;
  if (!p.begin(kNvsSpace, true)) return false;
  ssid = p.getString(kNvsSsid, "");
  pass = p.getString(kNvsPass, "");
  p.end();
  return ssid.length() > 0;
}

bool A7672Firebase::wifiSalvar(const String& ssid, const String& pass) {
  Preferences p;
  if (!p.begin(kNvsSpace, false)) return false;
  p.putString(kNvsSsid, ssid);
  p.putString(kNvsPass, pass);
  p.end();
  return true;
}

void A7672Firebase::wifiApagar() {
  Preferences p;
  if (!p.begin(kNvsSpace, false)) return;
  p.remove(kNvsSsid);
  p.remove(kNvsPass);
  p.end();
}

bool A7672Firebase::remoteLog(const String& level, const String& message) {
  // Escapa o que quebraria o JSON. As mensagens vêm do próprio firmware, mas
  // muitas carregam resposta do módulo, com aspas e barras dentro.
  String safe;
  safe.reserve(message.length() + 16);
  for (size_t i = 0; i < message.length(); i++) {
    char c = message[i];
    if (c == '"' || c == '\\') { safe += '\\'; safe += c; }
    else if (c == '\n' || c == '\r') safe += ' ';
    else if ((uint8_t)c < 0x20) continue;
    else safe += c;
  }
  if (safe.length() > 180) safe = safe.substring(0, 180);

  String json = "{\"lvl\":\"" + level + "\",\"msg\":\"" + safe +
                "\",\"up\":" + String(millis() / 1000) + "}";
  return put("/devices/" + _deviceId + "/logs.json", json, "POST");
}

bool A7672Firebase::sendStatus(const String& utc, bool hasFix, int sats, int rssi,
                               uint32_t sent, uint32_t failed) {
  String json = "{";
  json += "\"enabled\":" + String(_enabled ? "true" : "false");
  json += ",\"fix\":"    + String(hasFix ? "true" : "false");
  json += ",\"sats\":"   + String(sats);
  json += ",\"rssi\":"   + String(rssi);
  json += ",\"sent\":"   + String(sent);
  json += ",\"failed\":" + String(failed);
  json += ",\"up\":"     + String(millis() / 1000);
  if (utc.length()) json += ",\"seen\":\"" + utc + "\"";
  if (_uid.length()) json += ",\"uid\":\"" + _uid + "\"";
  json += "}";
  return put("/devices/" + _deviceId + "/status.json", json, "PUT");
}

// "2026-08-18T13:45:02Z" -> "2026-08-18". Sem data do satélite os pontos vão
// para um nó à parte em vez de se perderem ou contaminarem um dia real: isso
// acontece nos primeiros quadros NMEA, quando já há posição mas ainda não veio
// o RMC com a data.
String A7672Firebase::dayKey(const String& iso) {
  if (iso.length() < 10) return "sem-data";
  return iso.substring(0, 10);
}

// Distância em metros entre dois pontos (Haversine).
static double metrosEntre(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371000.0, rad = PI / 180.0;
  double dLat = (lat2 - lat1) * rad, dLon = (lon2 - lon1) * rad;
  double a = sin(dLat / 2) * sin(dLat / 2)
           + cos(lat1 * rad) * cos(lat2 * rad) * sin(dLon / 2) * sin(dLon / 2);
  return 2 * R * asin(sqrt(a));
}

bool A7672Firebase::sendFix(const GnssFix& fix) {
  if (!fix.valid) {
    _lastError = "Sem fix valido";
    return false;
  }

  // Parado, o GPS oscila alguns metros e cada oscilação viraria um ponto novo:
  // um aparelho na bancada gerava ~2.880 escritas por dia sem sair do lugar.
  // O filtro compara com o último ponto ENVIADO, não com o anterior, senão
  // uma deriva lenta passaria ponto a ponto.
  if (_minDist > 0 && _temUltimo) {
    double d = metrosEntre(_lastLat, _lastLon, fix.lat, fix.lon);
    if (d < _minDist) {
      _lastError = "Parado (" + String(d, 1) + " m < " + String(_minDist, 0) + " m)";
      return false;
    }
  }

  if (!ensureAuth()) return false;

  String utc = isoTimestamp(fix.utcDate, fix.utcTime);

  // Campo ausente é omitido, não zerado. toFloat() devolve 0.0 para campo
  // vazio do NMEA, e gravar "alt":0.0 afirmaria nível do mar — que num fix 2D
  // é invenção. O app faz a mesma omissão; manter os dois iguais evita que a
  // mesma trilha mude de forma dependendo de quem a gravou.
  String json = "{";
  json += "\"lat\":"   + String(fix.lat, 6);
  json += ",\"lon\":"  + String(fix.lon, 6);
  if (utc.length())    json += ",\"utc\":\"" + utc + "\"";
  if (fix.mode == 3)   json += ",\"alt\":"  + String(fix.altitude, 1);
  json += ",\"kmh\":"  + String(fix.speedKmh, 1);
  if (fix.svTotal)     json += ",\"sats\":" + String(fix.svTotal);
  if (fix.hdop > 0)    json += ",\"hdop\":" + String(fix.hdop, 2);
  json += "}";

  String base = "/devices/" + _deviceId;

  // A última posição sobrescreve (PUT); o histórico acumula (POST gera a chave)
  // dentro do nó do dia, de modo que cada data vira um bloco separado.
  bool okLast = put(base + "/last.json", json, "PUT");

  // Só conta como enviado o que de fato subiu: marcar antes faria uma falha de
  // rede "consumir" o deslocamento e o ponto seguinte seria filtrado à toa.
  if (okLast) { _lastLat = fix.lat; _lastLon = fix.lon; _temUltimo = true; }

  if (!_track) return okLast;

  bool okTrack = put(base + "/track/" + dayKey(utc) + ".json", json, "POST");
  return okLast && okTrack;
}
