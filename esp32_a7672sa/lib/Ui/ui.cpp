#include "ui.h"
#include <display.h>
#include <osm.h>

Ui ui;

// Paleta escura, para o display não ofuscar à noite dentro do carro.
static const uint16_t COR_FUNDO   = 0x0861;   // quase preto, levemente azul
static const uint16_t COR_BARRA   = 0x18E3;
static const uint16_t COR_LINHA   = 0x2965;
static const uint16_t COR_TEXTO   = 0xF79E;
static const uint16_t COR_FRACO   = 0x8410;
static const uint16_t COR_OK      = 0x2F6C;   // verde
static const uint16_t COR_ALERTA  = 0xFD20;   // âmbar
static const uint16_t COR_ERRO    = 0xF965;   // vermelho
static const uint16_t COR_DESTAQUE= 0x3D7F;   // azul

static const int LARG = 480, ALT = 320;
static const int BARRA_H = 28;
static const int RODAPE_Y = 240, RODAPE_H = ALT - RODAPE_Y;
static const int MAPA_Y = BARRA_H, MAPA_H = RODAPE_Y - BARRA_H;

// Rastro mostrado no mapa. Curto de propósito: o que interessa na tela é o
// trecho recente, e cada ponto guardado é RAM que o decodificador de PNG usa.
static const int MAX_RASTRO = 120;
static double rastroLat[MAX_RASTRO], rastroLon[MAX_RASTRO];
static int rastroN = 0;

static void addRastro(double lat, double lon) {
  if (rastroN && fabs(rastroLat[rastroN-1] - lat) < 1e-6
              && fabs(rastroLon[rastroN-1] - lon) < 1e-6) return;
  if (rastroN >= MAX_RASTRO) {
    memmove(rastroLat, rastroLat + 1, sizeof(double) * (MAX_RASTRO - 1));
    memmove(rastroLon, rastroLon + 1, sizeof(double) * (MAX_RASTRO - 1));
    rastroN = MAX_RASTRO - 1;
  }
  rastroLat[rastroN] = lat; rastroLon[rastroN] = lon; rastroN++;
}

void Ui::begin() {
  tft.init();
  // 3 = paisagem com o conector para o lado oposto ao da rotação 1. Com 1 a
  // imagem saía de cabeça para baixo nesta montagem.
  tft.setRotation(3);
  tft.fillScreen(COR_FUNDO);
  tft.setTextColor(COR_TEXTO, COR_FUNDO);
  _primeira = true;
}

void Ui::splash(const char* linha1, const char* linha2) {
  tft.fillScreen(COR_FUNDO);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COR_DESTAQUE, COR_FUNDO);
  tft.drawString(linha1, LARG / 2, ALT / 2 - 16, 4);
  if (linha2) {
    tft.setTextColor(COR_FRACO, COR_FUNDO);
    tft.drawString(linha2, LARG / 2, ALT / 2 + 16, 2);
  }
  tft.setTextDatum(TL_DATUM);
  _primeira = true;
}

void Ui::mensagem(const char* texto, uint16_t cor) {
  tft.fillRect(0, ALT / 2 - 22, LARG, 44, COR_BARRA);
  tft.drawFastHLine(0, ALT / 2 - 22, LARG, cor);
  tft.drawFastHLine(0, ALT / 2 + 22, LARG, cor);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(cor, COR_BARRA);
  tft.drawString(texto, LARG / 2, ALT / 2, 4);
  tft.setTextDatum(TL_DATUM);
  _primeira = true;
}

// Barrinhas de sinal: cinco níveis entre -110 e -55 dBm.
static void barrinhas(int x, int y, int dbm, bool online) {
  int nivel = online ? constrain((dbm + 110) / 11, 0, 5) : 0;
  for (int i = 0; i < 5; i++) {
    int h = 4 + i * 3;
    uint16_t c = (i < nivel) ? (nivel <= 1 ? COR_ERRO : nivel <= 2 ? COR_ALERTA : COR_OK)
                             : COR_LINHA;
    tft.fillRect(x + i * 5, y + 16 - h, 3, h, c);
  }
}

void Ui::barraStatus(const UiEstado& e) {
  // Repintar a faixa inteira a cada atualização é o que faz a tela piscar.
  // Só na primeira vez; depois cada texto se apaga sozinho pelo padding.
  if (_primeira) {
    tft.fillRect(0, 0, LARG, BARRA_H, COR_BARRA);
    tft.drawFastHLine(0, BARRA_H - 1, LARG, COR_LINHA);
  }
  tft.setTextDatum(TL_DATUM);

  // GNSS
  uint16_t cg = e.fix ? COR_OK : COR_ALERTA;
  tft.fillCircle(12, 14, 5, cg);
  tft.setTextColor(COR_TEXTO, COR_BARRA);
  char buf[40];
  if (e.fix) snprintf(buf, sizeof(buf), "GNSS %dD  %d sat", e.mode, e.sats);
  else       snprintf(buf, sizeof(buf), "GNSS buscando...");
  tft.setTextPadding(160);          // apaga o texto anterior sem fillRect
  tft.drawString(buf, 24, 7, 2);

  // Rede
  barrinhas(196, 6, e.dbm, e.online);
  tft.setTextColor(e.online ? COR_TEXTO : COR_FRACO, COR_BARRA);
  snprintf(buf, sizeof(buf), "%s %d dBm", e.online ? e.tech.c_str() : "sem rede",
           e.online ? e.dbm : 0);
  tft.setTextPadding(115);
  tft.drawString(buf, 226, 7, 2);

  // Envio ao Firebase
  uint16_t ce = e.envioAtivo ? COR_OK : COR_FRACO;
  tft.fillRoundRect(348, 5, 58, 18, 4, ce);
  tft.setTextColor(COR_BARRA, ce);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(e.envioAtivo ? "ENVIO" : "PARADO", 377, 14, 1);

  // Relógio do satélite (o RTC do ESP32 zera a cada reset)
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(COR_TEXTO, COR_BARRA);
  String hora = e.utc.length() >= 16 ? e.utc.substring(11, 16) : "--:--";
  tft.setTextPadding(48);
  tft.drawString(hora, LARG - 6, 7, 2);
  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);
}

void Ui::rodape(const UiEstado& e) {
  char buf[48];

  // Fundo e rótulos fixos só uma vez. Repintar tudo a cada meio segundo era o
  // que fazia o rodapé piscar — e ele muda a cada fix, porque o GPS oscila.
  if (_primeira) {
    tft.fillRect(0, RODAPE_Y, LARG, RODAPE_H, COR_BARRA);
    tft.drawFastHLine(0, RODAPE_Y, LARG, COR_LINHA);
    tft.setTextColor(COR_FRACO, COR_BARRA);
    tft.setTextDatum(TC_DATUM);
    tft.drawString("ALT", 250, RODAPE_Y + 6, 1);
    tft.drawString("VEL", 250, RODAPE_Y + 42, 1);
    tft.setTextDatum(TR_DATUM);
    tft.drawString("ENVIADOS / FALHAS", LARG - 10, RODAPE_Y + 6, 1);
    tft.setTextColor(COR_FRACO, COR_BARRA);
    tft.setTextDatum(TR_DATUM);
    tft.drawString("/", LARG - 62, RODAPE_Y + 20, 2);
  }

  // Coordenadas em destaque: é o dado que se quer ler de relance.
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(e.fix ? COR_TEXTO : COR_FRACO, COR_BARRA);
  tft.setTextPadding(200);
  snprintf(buf, sizeof(buf), "%s%.6f", e.lat >= 0 ? " " : "", e.lat);
  tft.drawString(buf, 10, RODAPE_Y + 8, 4);
  snprintf(buf, sizeof(buf), "%s%.6f", e.lon >= 0 ? " " : "", e.lon);
  tft.drawString(buf, 10, RODAPE_Y + 44, 4);

  // Altitude e velocidade
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COR_TEXTO, COR_BARRA);
  tft.setTextPadding(110);
  snprintf(buf, sizeof(buf), "%.0f m", e.alt);
  tft.drawString(buf, 250, RODAPE_Y + 16, 4);
  snprintf(buf, sizeof(buf), "%.0f km/h", e.kmh);
  tft.drawString(buf, 250, RODAPE_Y + 52, 4);

  // Contadores do Firebase
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(COR_OK, COR_BARRA);
  tft.setTextPadding(56);
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)e.enviados);
  tft.drawString(buf, LARG - 74, RODAPE_Y + 16, 4);
  tft.setTextColor(e.falhas ? COR_ERRO : COR_FRACO, COR_BARRA);
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)e.falhas);
  tft.drawString(buf, LARG - 10, RODAPE_Y + 16, 4);

  // HDOP diz se dá para confiar na posição
  tft.setTextColor(COR_FRACO, COR_BARRA);
  tft.setTextPadding(90);
  snprintf(buf, sizeof(buf), "HDOP %.1f", e.hdop);
  tft.drawString(buf, LARG - 10, RODAPE_Y + 56, 2);

  tft.setTextPadding(0);
  tft.setTextDatum(TL_DATUM);
}

void Ui::marcador(int cx, int cy, float curso) {
  // Halo claro para o marcador não sumir sobre ruas claras do mapa.
  tft.fillCircle(cx, cy, 9, 0x0000);
  tft.fillCircle(cx, cy, 8, COR_DESTAQUE);
  tft.fillCircle(cx, cy, 4, 0xFFFF);
  tft.drawCircle(cx, cy, 12, COR_DESTAQUE);
}

void Ui::trilhaNoMapa(const UiEstado& e) {
  if (rastroN < 2) return;
  int px, py;
  OsmMap::pixelNoTile(e.lat, e.lon, _zoom, px, py);
  TileXY c = OsmMap::tileDe(e.lat, e.lon, _zoom);
  double n = pow(2.0, _zoom);

  // Converte cada ponto para pixel na tela, usando o mesmo referencial do
  // tile central que o mapa desenhou.
  auto paraTela = [&](double lat, double lon, int& sx, int& sy) {
    double latRad = lat * M_PI / 180.0;
    double fx = (lon + 180.0) / 360.0 * n;
    double fy = (1.0 - log(tan(latRad) + 1.0 / cos(latRad)) / M_PI) / 2.0 * n;
    sx = (int)((fx - c.x) * 256.0) + (LARG / 2 - px);
    sy = (int)((fy - c.y) * 256.0) + (MAPA_Y + MAPA_H / 2 - py);
  };

  int ax, ay, bx, by;
  paraTela(rastroLat[0], rastroLon[0], ax, ay);
  for (int i = 1; i < rastroN; i++) {
    paraTela(rastroLat[i], rastroLon[i], bx, by);
    // Só traça o segmento se ao menos uma ponta cair na área do mapa.
    bool dentro = (ay >= MAPA_Y && ay < RODAPE_Y) || (by >= MAPA_Y && by < RODAPE_Y);
    if (dentro) {
      tft.drawLine(ax, ay, bx, by, COR_ALERTA);
      tft.drawLine(ax, ay + 1, bx, by + 1, COR_ALERTA);
    }
    ax = bx; ay = by;
  }
}

void Ui::desenharMapa(const UiEstado& e) {
  if (!e.fix) {
    tft.fillRect(0, MAPA_Y, LARG, MAPA_H, COR_FUNDO);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COR_FRACO, COR_FUNDO);
    tft.drawString("aguardando fix do GNSS", LARG / 2, MAPA_Y + MAPA_H / 2 - 10, 4);
    tft.drawString("30 a 90 s a frio, com vista para o ceu", LARG / 2, MAPA_Y + MAPA_H / 2 + 20, 2);
    tft.setTextDatum(TL_DATUM);
    return;
  }

  int faltando = mapa.desenhar(0, MAPA_Y, LARG, MAPA_H, e.lat, e.lon, _zoom);
  trilhaNoMapa(e);
  marcador(LARG / 2, MAPA_Y + MAPA_H / 2, 0);

  // Escala: sem ela não dá para saber se o traço na tela são 50 m ou 5 km.
  double metrosPorPixel = 156543.03392 * cos(e.lat * M_PI / 180.0) / pow(2.0, _zoom);
  int barraPx = 80;
  int metros = (int)(metrosPorPixel * barraPx);
  char buf[24];
  if (metros >= 1000) snprintf(buf, sizeof(buf), "%.1f km", metros / 1000.0);
  else                snprintf(buf, sizeof(buf), "%d m", metros);
  int by = RODAPE_Y - 14;
  tft.drawFastHLine(12, by, barraPx, 0xFFFF);
  tft.drawFastVLine(12, by - 4, 8, 0xFFFF);
  tft.drawFastVLine(12 + barraPx, by - 4, 8, 0xFFFF);
  tft.setTextDatum(BL_DATUM);
  tft.setTextColor(0xFFFF, COR_FUNDO);
  tft.drawString(buf, 12 + barraPx + 6, by + 5, 2);

  // Estado do mapa: sem Wi-Fi e sem cache, é isso que explica a grade vazia.
  tft.setTextDatum(TR_DATUM);
  if (faltando > 0 && !e.wifi) {
    tft.setTextColor(COR_ALERTA, COR_FUNDO);
    snprintf(buf, sizeof(buf), "offline  %d tiles", faltando);
    tft.drawString(buf, LARG - 8, MAPA_Y + 6, 2);
  } else if (faltando > 0) {
    tft.setTextColor(COR_DESTAQUE, COR_FUNDO);
    snprintf(buf, sizeof(buf), "baixando  %d", faltando);
    tft.drawString(buf, LARG - 8, MAPA_Y + 6, 2);
  }
  tft.setTextDatum(TL_DATUM);
}

void Ui::atualizar(const UiEstado& e) {
  if (e.fix) addRastro(e.lat, e.lon);

  bool mudouBarra = _primeira
      || e.fix != _ant.fix || e.sats != _ant.sats || e.mode != _ant.mode
      || e.online != _ant.online || e.dbm != _ant.dbm
      || e.envioAtivo != _ant.envioAtivo
      || e.utc.substring(0, 16) != _ant.utc.substring(0, 16);

  bool mudouRodape = _primeira
      || fabs(e.lat - _ant.lat) > 1e-6 || fabs(e.lon - _ant.lon) > 1e-6
      || (int)e.alt != (int)_ant.alt || (int)e.kmh != (int)_ant.kmh
      || e.enviados != _ant.enviados || e.falhas != _ant.falhas;

  // Redesenhar o mapa custa caro (lê e decodifica PNG do flash), então só
  // quando a posição sai do miolo da tela ou o zoom muda.
  double distGraus = sqrt(pow(e.lat - _mapaLat, 2) + pow(e.lon - _mapaLon, 2));
  bool mudouMapa = _primeira || _mapaSujo
      || (e.fix && distGraus > 0.0004)
      || (e.fix != _ant.fix);

  if (mudouBarra) barraStatus(e);

  // O marcador fica sempre no centro (o mapa é que se move sob ele), então
  // redesenhá-lo de segundo em segundo só piscava sem mudar nada na tela.
  if (mudouMapa) {
    desenharMapa(e);
    _mapaLat = e.lat; _mapaLon = e.lon;
    _mapaSujo = false;
    _ultimoMapa = millis();
  }
  if (mudouRodape) rodape(e);

  _ant = e;
  _primeira = false;
}
