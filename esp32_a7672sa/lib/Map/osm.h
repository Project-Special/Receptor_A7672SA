// Mapa de tiles do OpenStreetMap no display.
//
// Os tiles vêm SÓ pelo Wi-Fi e ficam gravados no LittleFS. O chip do A7672 é
// um SIM IoT com franquia pequena: uma tela cheia são 6 a 12 tiles de ~15 KB,
// e arrastar o mapa multiplicaria isso. A ideia é carregar a região em casa,
// com Wi-Fi, e na rua desenhar do cache — sem gastar um byte de dados móveis.
//
// Sem Wi-Fi e sem cache, o mapa não fica em branco: a área vira uma grade com
// escala, e a trilha é desenhada por cima. Ver Ui::desenharMapa().
#pragma once

#include <Arduino.h>
#include <display.h>

struct TileXY {
  int32_t x = 0, y = 0;
  int     z = 15;
};

class OsmMap {
public:
  // Monta o cache. Sem LittleFS o mapa continua funcionando, só não guarda
  // nada entre reinícios.
  bool begin();

  // Conversões da projeção Web Mercator (o "slippy map" do OSM).
  static TileXY tileDe(double lat, double lon, int z);
  static double lonDoTile(double x, int z);
  static double latDoTile(double y, int z);

  // Posição do pixel dentro do tile, em 0..255.
  static void pixelNoTile(double lat, double lon, int z, int& px, int& py);

  // Desenha a área centrada em (lat,lon). Devolve quantos tiles faltaram —
  // esses aparecem como grade, e é o que o rótulo "offline" mostra.
  int desenhar(int x0, int y0, int w, int h, double lat, double lon, int z);

  // Baixa os tiles que faltam para cobrir a área. Só age com Wi-Fi ligado.
  // `maxTiles` limita o quanto cada chamada segura o loop.
  int precarregar(double lat, double lon, int z, int raio = 1, int maxTiles = 4);

  bool temCache(const TileXY& t) const;
  size_t tilesEmCache() const { return _emCache; }
  const String& ultimoErro() const { return _erro; }

private:
  static String caminho(const TileXY& t);
  bool baixar(const TileXY& t);
  bool desenharTile(const TileXY& t, int px, int py);
  void grade(int x0, int y0, int w, int h);

  size_t _emCache = 0;
  String _erro;
  bool   _fsOk = false;
};

extern OsmMap mapa;
