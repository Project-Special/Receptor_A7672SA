#include "cfg.h"
#include <Preferences.h>

CfgUnidade cfg;

static const char* kNs = "unidade";

// Extratores mínimos: o JSON aqui é o que o app manda, com meia dúzia de
// campos rasos. Uma lib completa custaria RAM sem resolver nada além.
static String valorCru(const String& src, const String& chave) {
  String alvo = "\"" + chave + "\"";
  int k = src.indexOf(alvo);
  if (k < 0) return "";
  int dois = src.indexOf(':', k + alvo.length());
  if (dois < 0) return "";

  int i = dois + 1;
  while (i < (int)src.length() && (src[i] == ' ' || src[i] == '"')) i++;
  int ini = i;
  while (i < (int)src.length() && src[i] != ',' && src[i] != '}' && src[i] != '"') i++;

  String v = src.substring(ini, i);
  v.trim();
  return v;
}

static bool paraBool(const String& v, bool atual) {
  if (v == "true" || v == "1")  return true;
  if (v == "false" || v == "0") return false;
  return atual;
}

// Identifica o binário. Muda a cada compilação, que é justamente o gatilho:
// firmware novo gravado = configuração de fábrica.
static const char* kBuild = __DATE__ " " __TIME__;

void CfgUnidade::restaurarPadrao() {
  *this = CfgUnidade{};    // os defaults declarados no cabeçalho
  salvar();
}

void CfgUnidade::carregar() {
  {
    Preferences p;
    if (p.begin(kNs, false)) {
      String gravado = p.getString("build", "");
      if (gravado != kBuild) {
        p.end();
        restaurarPadrao();
        Preferences q;
        if (q.begin(kNs, false)) { q.putString("build", kBuild); q.end(); }
        return;                      // nada de ler o que ficou da versão antiga
      }
      p.end();
    }
  }

  Preferences p;
  if (!p.begin(kNs, true)) return;
  enviar     = p.getBool("enviar", enviar);
  historico  = p.getBool("hist", historico);
  display    = p.getBool("display", display);
  ble        = p.getBool("ble", ble);
  intervalo  = p.getUInt("intv", intervalo);
  statusCada = p.getUInt("stat", statusCada);
  distMin    = p.getFloat("dist", distMin);
  tzMin      = p.getInt("tz", tzMin);
  p.end();
}

void CfgUnidade::salvar() const {
  Preferences p;
  if (!p.begin(kNs, false)) return;
  p.putBool("enviar", enviar);
  p.putBool("hist", historico);
  p.putBool("display", display);
  p.putBool("ble", ble);
  p.putUInt("intv", intervalo);
  p.putUInt("stat", statusCada);
  p.putFloat("dist", distMin);
  p.putInt("tz", tzMin);
  p.end();
}

String CfgUnidade::paraJson() const {
  String j = "{";
  j += "\"enviar\":"    + String(enviar ? "true" : "false");
  j += ",\"historico\":" + String(historico ? "true" : "false");
  j += ",\"display\":"   + String(display ? "true" : "false");
  j += ",\"ble\":"       + String(ble ? "true" : "false");
  j += ",\"intervalo\":" + String(intervalo);
  j += ",\"statusCada\":" + String(statusCada);
  // String(float, 0) prefixa um espaço; aqui vira inteiro mesmo — distância
  // mínima em fração de metro não significa nada num GPS.
  j += ",\"distMin\":"   + String((int)distMin);
  j += ",\"tz\":"        + String(tzMin);
  j += "}";
  return j;
}

bool CfgUnidade::aplicarJson(const String& json) {
  CfgUnidade antes = *this;
  String v;

  if ((v = valorCru(json, "enviar")).length())    enviar    = paraBool(v, enviar);
  if ((v = valorCru(json, "historico")).length()) historico = paraBool(v, historico);
  if ((v = valorCru(json, "display")).length())   display   = paraBool(v, display);
  if ((v = valorCru(json, "ble")).length())       ble       = paraBool(v, ble);

  // Limites conferidos aqui porque agora não há regra de banco no caminho: um
  // intervalo de 99999 s deixaria a unidade muda por 27 horas.
  if ((v = valorCru(json, "intervalo")).length())
    intervalo = constrain((uint32_t)v.toInt(), 5U, 3600U);
  if ((v = valorCru(json, "statusCada")).length())
    statusCada = constrain((uint32_t)v.toInt(), 10U, 3600U);
  if ((v = valorCru(json, "distMin")).length())
    distMin = constrain(v.toFloat(), 0.0f, 10000.0f);
  if ((v = valorCru(json, "tz")).length())
    tzMin = constrain((int)v.toInt(), -720, 840);

  bool mudou = enviar != antes.enviar || historico != antes.historico
            || display != antes.display || ble != antes.ble
            || intervalo != antes.intervalo
            || statusCada != antes.statusCada || distMin != antes.distMin
            || tzMin != antes.tzMin;
  if (mudou) salvar();
  return mudou;
}
