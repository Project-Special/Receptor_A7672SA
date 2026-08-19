// Tela do rastreador: barra de status, mapa e rodapé com a telemetria.
//
// Layout em 480x320:
//
//   +--------------------------------------------------+  0
//   | GNSS 3D  9 sat |  LTE -55 dBm  |  ENVIO  | 14:32 |  barra 28 px
//   +--------------------------------------------------+  28
//   |                                                  |
//   |                 mapa (tiles OSM)                 |  212 px
//   |                     (o) voce                     |
//   |                                                  |
//   +--------------------------------------------------+  240
//   | -29.181796                765 m       12 enviados|  rodapé 80 px
//   | -51.215696                0 km/h      0 falhas   |
//   +--------------------------------------------------+  320
//
// Só redesenha o que mudou: repintar a tela inteira a cada fix faria o SPI
// disputar tempo com o módulo e a imagem piscaria.
#pragma once

#include <Arduino.h>
#include <gnss.h>
#include <net.h>

struct UiEstado {
  bool     fix = false;
  int      mode = 0;
  double   lat = 0, lon = 0;
  float    alt = 0, kmh = 0, hdop = 0;
  int      sats = 0;
  String   utc;

  String   operadora, tech;
  int      dbm = 0;
  bool     online = false;

  bool     envioAtivo = false;
  uint32_t enviados = 0, falhas = 0;

  bool     wifi = false;
  int      tilesFaltando = 0;
  size_t   tilesCache = 0;
};

class Ui {
public:
  void begin();

  // Redesenha o que mudou desde a última chamada.
  void atualizar(const UiEstado& e);

  // Repinta tudo — usar depois de sair de uma tela cheia (splash, alerta).
  void forcarRedesenho() { _primeira = true; }

  void splash(const char* linha1, const char* linha2 = nullptr);
  void mensagem(const char* texto, uint16_t cor);

  int  zoom() const { return _zoom; }
  void setZoom(int z) { _zoom = constrain(z, 3, 18); _mapaSujo = true; }
  void marcarMapaSujo() { _mapaSujo = true; }

private:
  void barraStatus(const UiEstado& e);
  void rodape(const UiEstado& e);
  void desenharMapa(const UiEstado& e);
  void marcador(int cx, int cy, float curso);
  void trilhaNoMapa(const UiEstado& e);

  UiEstado _ant;
  bool _primeira = true;
  bool _mapaSujo = true;
  int  _zoom = 16;
  double _mapaLat = 0, _mapaLon = 0;
  uint32_t _ultimoMapa = 0;
};

extern Ui ui;
