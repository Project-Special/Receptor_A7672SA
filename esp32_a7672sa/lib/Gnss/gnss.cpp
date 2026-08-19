#include "gnss.h"

static String field(const String& s, int idx) {
  int start = 0, f = 0;
  for (int i = 0; i <= (int)s.length(); i++) {
    if (i == (int)s.length() || s[i] == ',') {
      if (f == idx) return s.substring(start, i);
      f++; start = i + 1;
    }
  }
  return "";
}

// ddmm.mmmm / dddmm.mmmm -> graus decimais
double A7672Gnss::nmeaToDegrees(const String& value, char hemi) {
  if (value.length() < 3) return 0;
  int dot = value.indexOf('.');
  if (dot < 2) return 0;
  int degDigits = dot - 2;
  double deg = value.substring(0, degDigits).toDouble();
  double min = value.substring(degDigits).toDouble();
  double out = deg + min / 60.0;
  if (hemi == 'S' || hemi == 'W') out = -out;
  return out;
}

bool A7672Gnss::powerOn(int mode, uint32_t readyTimeoutMs) {
  if (!_urcHooked) {
    _urcHooked = true;
    _c.addUrcHandler([this](const String& line) {
      if (line.length() > 1 && (line[0] == '$')) feedNmea(line);
    });
  }

  // Reenviar CGNSSPWR=1 num receptor já ligado REINICIA o motor e derruba um
  // fix válido — custa outros 30-90 s de reaquisição por nada.
  if (isPowered()) {
    setPortRouting();
    return true;
  }

  if (!_c.at("AT+CGNSSPWR=1", 12000)) return false;    // variante sem GNSS dá ERROR

  // O receptor só aceita configuração depois desta URC.
  if (!_c.waitToken("+CGNSSPWR: READY", readyTimeoutMs)) return false;

  _c.at("AT+CGNSSMODE=" + String(mode));
  setPortRouting();                 // sem isto o +CGNSSINFO pode vir vazio
  return true;
}

bool A7672Gnss::powerOff() {
  _fix = GnssFix();
  return _c.at("AT+CGNSSPWR=0", 10000);
}

bool A7672Gnss::isPowered() {
  String r = _c.queryLine("AT+CGNSSPWR?", "+CGNSSPWR:", 5000);
  if (!r.length()) return false;
  String v = r.substring(r.indexOf(':') + 1);
  v.trim();
  return v.startsWith("1");
}

bool A7672Gnss::readFix(GnssFix& out) {
  String r = _c.queryLine("AT+CGNSSINFO", "+CGNSSINFO:", 8000);
  if (!r.length()) return false;

  String b = r.substring(r.indexOf(':') + 1);
  b.trim();

  // Sem fix o módulo devolve os campos vazios: '+CGNSSINFO: ,,,,,,,,'
  String latRaw = field(b, 4), lonRaw = field(b, 6);
  if (!latRaw.length() || !lonRaw.length()) { out.valid = false; return true; }

  out.valid     = true;
  out.mode      = field(b, 0).toInt();
  out.svGps     = field(b, 1).toInt();
  out.svGlonass = field(b, 2).toInt();
  out.svBeidou  = field(b, 3).toInt();
  out.svTotal   = out.svGps + out.svGlonass + out.svBeidou;
  out.lat       = nmeaToDegrees(latRaw, field(b, 5).length() ? field(b, 5)[0] : 'N');
  out.lon       = nmeaToDegrees(lonRaw, field(b, 7).length() ? field(b, 7)[0] : 'E');
  out.utcDate   = field(b, 8);
  out.utcTime   = field(b, 9);
  out.altitude  = field(b, 10).toFloat();
  out.speedKmh  = field(b, 11).toFloat() * 1.852f;   // nós -> km/h
  out.course    = field(b, 12).toFloat();
  out.pdop      = field(b, 13).toFloat();
  out.hdop      = field(b, 14).toFloat();
  out.vdop      = field(b, 15).toFloat();
  out.updatedAt = millis();
  _fix = out;
  return true;
}

bool A7672Gnss::startNmea(int rateHz, bool detalhado) {
  if (!setPortRouting()) return false;

  // Ordem dos campos: GGA,GLL,GSA,GSV,RMC,VTG,ZDA,GST.
  //
  // Medido no stream real, GSV e GLL somam 52% dos ~625 B/s que o receptor
  // despeja na UART — e essa é a mesma UART por onde passam os comandos AT.
  // Com ela saturada, respostas de AT+CGATT? e do canal SSL se perdiam no meio
  // do NMEA e derrubavam envios que teriam funcionado.
  //
  // GLL é redundante (RMC já traz posição e hora) e GSV só serve para detalhar
  // satélites por constelação; a contagem em uso vem do GGA. Ligue `detalhado`
  // quando essa lista importar mais que a folga na UART.
  _c.at(detalhado ? "AT+CGNSSNMEA=1,1,1,1,1,0,0,0"
                  : "AT+CGNSSNMEA=1,0,1,0,1,0,0,0");
  _c.at("AT+CGPSNMEARATE=" + String(rateHz));
  return _c.at("AT+CGNSSTST=1");
}

// AT+CGNSSPORTSWITCH=<porta_dados_parseados>,<porta_nmea>, onde 0 = USB e
// 1 = UART. Rotear os dados parseados para a porta errada faz o +CGNSSINFO
// responder vazio mesmo com fix 3D válido — o dado sai pela outra porta.
bool A7672Gnss::setPortRouting() {
  return _c.at("AT+CGNSSPORTSWITCH=" + String(_parsedPort) + "," + String(_nmeaPort));
}

bool A7672Gnss::stopNmea() { return _c.at("AT+CGNSSTST=0"); }

// Um quadro chega fatiado em ~15 sentenças por segundo; cada tipo traz um
// pedaço. Acumulamos em _fix.
void A7672Gnss::feedNmea(const String& line) {
  int star = line.indexOf('*');
  String body = (star > 0) ? line.substring(1, star) : line.substring(1);
  String type = body.substring(0, body.indexOf(','));
  if (type.length() < 5) return;
  String talker = type.substring(0, 2);
  String kind   = type.substring(type.length() - 3);

  if (kind == "GGA") {
    _fix.utcTime = field(body, 1);
    int quality  = field(body, 6).toInt();
    _fix.svTotal = field(body, 7).toInt();
    if (quality > 0) {
      String la = field(body, 2), lo = field(body, 4);
      if (la.length() && lo.length()) {
        _fix.lat = nmeaToDegrees(la, field(body, 3)[0]);
        _fix.lon = nmeaToDegrees(lo, field(body, 5)[0]);
        _fix.altitude = field(body, 9).toFloat();
        _fix.valid = true;
        _fix.updatedAt = millis();
      }
    } else _fix.valid = false;

  } else if (kind == "RMC") {
    _fix.utcTime = field(body, 1);
    _fix.utcDate = field(body, 9);
    if (field(body, 2) == "A") {
      _fix.speedKmh = field(body, 7).toFloat() * 1.852f;
      _fix.course   = field(body, 8).toFloat();
    }

  } else if (kind == "GSA") {
    _fix.mode = field(body, 2).toInt();
    _fix.pdop = field(body, 15).toFloat();
    _fix.hdop = field(body, 16).toFloat();
    _fix.vdop = field(body, 17).toFloat();

    // Satélites EM USO ficam nos campos 3..14. Vem uma GSA por constelação; o
    // último campo é o system id (NMEA 4.10+), com o talker como reserva.
    int used = 0;
    for (int i = 3; i <= 14; i++) if (field(body, i).length()) used++;

    int sysId = field(body, 18).toInt();
    if (sysId == 0) {
      if      (talker == "GP") sysId = 1;
      else if (talker == "GL") sysId = 2;
      else if (talker == "GA") sysId = 3;
      else if (talker == "GB" || talker == "BD") sysId = 4;
    }
    switch (sysId) {
      case 1: _fix.svGps     = used; break;
      case 2: _fix.svGlonass = used; break;
      case 3: _fix.svGalileo = used; break;
      case 4: _fix.svBeidou  = used; break;
      default: break;
    }
  }
}
