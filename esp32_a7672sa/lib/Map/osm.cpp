#include "osm.h"
#include <FS.h>          // define File; o LittleFS.h não o traz sozinho
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <PNGdec.h>
#include <math.h>

OsmMap mapa;

static const int  kTile = 256;
static const char kDir[] = "/tiles";

// O decodificador entrega uma linha por vez; estas variáveis dizem onde ela
// cai na tela. PNGdec não aceita contexto por ponteiro nas versões usadas aqui.
static PNG   png;
static int   gDestX = 0, gDestY = 0;
static int   gClipX0 = 0, gClipY0 = 0, gClipX1 = 0, gClipY1 = 0;

static int aoDesenharLinha(PNGDRAW* pDraw) {
  uint16_t linha[kTile];
  png.getLineAsRGB565(pDraw, linha, PNG_RGB565_BIG_ENDIAN, 0xffffffff);

  int y = gDestY + pDraw->y;
  if (y < gClipY0 || y >= gClipY1) return 1;   // fora da área: pula a linha

  // Recorta na horizontal: os tiles da borda entram pela metade na área do mapa.
  int x0 = gDestX, larg = pDraw->iWidth;
  int corteEsq = 0;
  if (x0 < gClipX0) { corteEsq = gClipX0 - x0; x0 = gClipX0; larg -= corteEsq; }
  if (x0 + larg > gClipX1) larg = gClipX1 - x0;
  if (larg <= 0) return 1;

  tft.pushImage(x0, y, larg, 1, linha + corteEsq);
  return 1;
}

bool OsmMap::begin() {
  _fsOk = LittleFS.begin(true);
  if (!_fsOk) { _erro = "LittleFS indisponivel"; return false; }
  if (!LittleFS.exists(kDir)) LittleFS.mkdir(kDir);

  // Conta o que já está guardado — é o que diz se dá para navegar offline.
  _emCache = 0;
  fs::File dz = LittleFS.open(kDir);
  for (fs::File z = dz.openNextFile(); z; z = dz.openNextFile()) {
    if (!z.isDirectory()) continue;
    fs::File dx = LittleFS.open(z.path());
    for (fs::File x = dx.openNextFile(); x; x = dx.openNextFile()) {
      if (!x.isDirectory()) continue;
      fs::File dy = LittleFS.open(x.path());
      for (fs::File y = dy.openNextFile(); y; y = dy.openNextFile()) _emCache++;
    }
  }
  return true;
}

// Fórmulas do slippy map: longitude é linear, latitude passa pela projeção
// de Mercator. Ver wiki.openstreetmap.org/wiki/Slippy_map_tilenames.
TileXY OsmMap::tileDe(double lat, double lon, int z) {
  TileXY t; t.z = z;
  double n = pow(2.0, z);
  double latRad = lat * M_PI / 180.0;
  t.x = (int32_t)floor((lon + 180.0) / 360.0 * n);
  t.y = (int32_t)floor((1.0 - log(tan(latRad) + 1.0 / cos(latRad)) / M_PI) / 2.0 * n);
  return t;
}

double OsmMap::lonDoTile(double x, int z) {
  return x / pow(2.0, z) * 360.0 - 180.0;
}

double OsmMap::latDoTile(double y, int z) {
  double n = M_PI - 2.0 * M_PI * y / pow(2.0, z);
  return 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
}

void OsmMap::pixelNoTile(double lat, double lon, int z, int& px, int& py) {
  double n = pow(2.0, z);
  double latRad = lat * M_PI / 180.0;
  double fx = (lon + 180.0) / 360.0 * n;
  double fy = (1.0 - log(tan(latRad) + 1.0 / cos(latRad)) / M_PI) / 2.0 * n;
  px = (int)((fx - floor(fx)) * kTile);
  py = (int)((fy - floor(fy)) * kTile);
}

String OsmMap::caminho(const TileXY& t) {
  return String(kDir) + "/" + t.z + "/" + t.x + "/" + t.y + ".png";
}

bool OsmMap::temCache(const TileXY& t) const {
  return _fsOk && LittleFS.exists(caminho(t));
}

bool OsmMap::baixar(const TileXY& t) {
  if (WiFi.status() != WL_CONNECTED) { _erro = "sem Wi-Fi"; return false; }
  if (!_fsOk) { _erro = "sem LittleFS"; return false; }
  if (temCache(t)) return true;

  WiFiClientSecure cli;
  cli.setInsecure();          // tile público; validar CA custaria RAM sem ganho
  cli.setTimeout(8000);

  HTTPClient http;
  String url = "https://tile.openstreetmap.org/" + String(t.z) + "/" + t.x + "/" + t.y + ".png";
  if (!http.begin(cli, url)) { _erro = "begin falhou"; return false; }
  // A política de uso do OSM exige identificar o cliente; sem User-Agent
  // próprio eles bloqueiam.
  http.setUserAgent("A7672SA-Tracker/1.0 (github.com/Project-Special)");

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    _erro = "HTTP " + String(code);
    http.end();
    return false;
  }

  LittleFS.mkdir(String(kDir) + "/" + t.z);
  LittleFS.mkdir(String(kDir) + "/" + t.z + "/" + t.x);

  // Grava em arquivo temporário e só então renomeia: um download interrompido
  // deixaria um PNG truncado no cache, e ele nunca mais seria rebaixado.
  String destino = caminho(t);
  String temp = destino + ".part";
  fs::File f = LittleFS.open(temp, "w");
  if (!f) { _erro = "nao abriu " + temp; http.end(); return false; }

  int escritos = http.writeToStream(&f);
  f.close();
  http.end();

  if (escritos <= 0) { LittleFS.remove(temp); _erro = "download vazio"; return false; }
  // Remover um arquivo que não existe faz o VFS logar erro a cada tile novo.
  if (LittleFS.exists(destino)) LittleFS.remove(destino);
  LittleFS.rename(temp, destino);
  _emCache++;
  _erro = "";
  return true;
}

bool OsmMap::desenharTile(const TileXY& t, int px, int py) {
  if (!temCache(t)) return false;

  fs::File f = LittleFS.open(caminho(t), "r");
  if (!f) return false;
  size_t n = f.size();
  if (!n) { f.close(); return false; }

  // O PNG inteiro precisa estar em RAM para o decodificador. ~15-25 KB por
  // tile: vai para a PSRAM, que sobra, em vez de disputar a RAM interna.
  uint8_t* buf = (uint8_t*)ps_malloc(n);
  if (!buf) { f.close(); return false; }
  f.read(buf, n);
  f.close();

  gDestX = px; gDestY = py;
  bool ok = (png.openRAM(buf, n, aoDesenharLinha) == PNG_SUCCESS);
  if (ok) { png.decode(nullptr, 0); png.close(); }
  free(buf);
  return ok;
}

void OsmMap::grade(int x0, int y0, int w, int h) {
  tft.fillRect(x0, y0, w, h, 0x18E3);           // cinza-azulado escuro
  for (int x = x0; x < x0 + w; x += 40) tft.drawFastVLine(x, y0, h, 0x2124);
  for (int y = y0; y < y0 + h; y += 40) tft.drawFastHLine(x0, y, w, 0x2124);
}

int OsmMap::desenhar(int x0, int y0, int w, int h, double lat, double lon, int z) {
  TileXY centro = tileDe(lat, lon, z);
  int px, py;
  pixelNoTile(lat, lon, z, px, py);

  // Canto superior esquerdo da área, em pixels do tile central.
  int origemX = x0 + w / 2 - px;
  int origemY = y0 + h / 2 - py;

  // Quantos tiles cobrem a área em cada direção.
  int antesX = (int)ceil((double)(origemX - x0) / kTile);
  int depoisX = (int)ceil((double)((x0 + w) - (origemX + kTile)) / kTile);
  int antesY = (int)ceil((double)(origemY - y0) / kTile);
  int depoisY = (int)ceil((double)((y0 + h) - (origemY + kTile)) / kTile);

  gClipX0 = x0; gClipY0 = y0; gClipX1 = x0 + w; gClipY1 = y0 + h;

  int faltando = 0;
  for (int dx = -antesX; dx <= depoisX; dx++) {
    for (int dy = -antesY; dy <= depoisY; dy++) {
      TileXY t{ centro.x + dx, centro.y + dy, z };
      int tx = origemX + dx * kTile, ty = origemY + dy * kTile;

      if (!desenharTile(t, tx, ty)) {
        faltando++;
        // Pinta só a parte visível do tile ausente, para não apagar vizinhos.
        int rx = max(tx, x0), ry = max(ty, y0);
        int rw = min(tx + kTile, x0 + w) - rx;
        int rh = min(ty + kTile, y0 + h) - ry;
        if (rw > 0 && rh > 0) grade(rx, ry, rw, rh);
      }
    }
  }
  return faltando;
}

int OsmMap::precarregar(double lat, double lon, int z, int raio, int maxTiles) {
  if (WiFi.status() != WL_CONNECTED) return 0;
  TileXY c = tileDe(lat, lon, z);
  int baixados = 0;
  for (int dx = -raio; dx <= raio && baixados < maxTiles; dx++) {
    for (int dy = -raio; dy <= raio && baixados < maxTiles; dy++) {
      TileXY t{ c.x + dx, c.y + dy, z };
      if (temCache(t)) continue;
      if (baixar(t)) baixados++;
    }
  }
  return baixados;
}
