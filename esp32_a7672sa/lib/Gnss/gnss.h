// GNSS: alimentação do receptor, +CGNSSINFO e parser NMEA.
// Não depende de SIM — o receptor é independente da rede celular.
#pragma once

#include <core.h>

struct GnssFix {
  bool   valid = false;
  int    mode  = 0;          // 2 = 2D, 3 = 3D
  double lat = 0, lon = 0;
  float  altitude = 0;
  float  speedKmh = 0;
  float  course = 0;
  float  pdop = 0, hdop = 0, vdop = 0;
  int    svGps = 0, svGlonass = 0, svGalileo = 0, svBeidou = 0;
  int    svTotal = 0;
  String utcDate;            // ddmmyy
  String utcTime;            // hhmmss
  uint32_t updatedAt = 0;    // millis() do último quadro completo
};

class A7672Gnss {
public:
  explicit A7672Gnss(A7672Core& core) : _c(core) {}

  // mode 3 = GPS+GLONASS na maioria dos firmwares. Aguarda a URC
  // '+CGNSSPWR: READY!' antes de configurar constelações: mandar o CGNSSMODE
  // antes disso faz o módulo responder ERROR.
  bool powerOn(int mode = 3, uint32_t readyTimeoutMs = 20000);
  bool powerOff();

  // Consulta pontual via +CGNSSINFO.
  bool readFix(GnssFix& out);

  // Stream NMEA na porta AT. Sem o PORTSWITCH as sentenças saem pela porta
  // dedicada e nunca chegam aqui.
  bool startNmea(int rateHz = 1);
  bool stopNmea();

  // Com o stream ligado, alimenta o fix a partir de RMC/GGA/GSA.
  const GnssFix& fix() const { return _fix; }

  static double nmeaToDegrees(const String& value, char hemi);

private:
  void feedNmea(const String& line);

  A7672Core& _c;
  GnssFix _fix;
  bool _urcHooked = false;
};
