// Demonstração do A7672SA no ESP32.
// Cada subsistema vive em lib/<Modulo>/ e recebe o núcleo por referência.
#include <Arduino.h>
#include <core.h>
#include <net.h>
#include <sms.h>
#include <voice.h>
#include <gnss.h>
#include <http.h>
#include <tls.h>
#include <firebase.h>
#include <WiFi.h>
#include <display.h>
#include <ui.h>
#include <osm.h>

// ── Pinos ────────────────────────────────────────────────────
// UART2 do ESP32 ligada à UART principal do módulo (115200 8N1).
static const int PIN_RX     = 16;   // ESP32 RX  <- TX do módulo
static const int PIN_TX     = 17;   // ESP32 TX  -> RX do módulo
static const int PIN_PWRKEY = 4;
static const int PIN_STATUS = 5;    // -1 se o STATUS do módulo não estiver ligado

// A placa Muz 24x24 inverte o PWRKEY com um transistor: o header é ativo em
// ALTO. Ligando direto no pino do módulo, troque para false (ativo em BAIXO).
static const bool PWRKEY_ACTIVE_HIGH = true;

// O chip da bancada é um SIM IoT emnify (IMSI 724-51), que faz roaming nacional
// na Claro mas usa APN próprio. Com um chip de operadora comum, troque para
// "claro.com.br" / "zap.vivo.com.br" / "tim.br".
static const char* APN = "em.mnc051.mcc724.gprs";

// ── Firebase ─────────────────────────────────────────────────
// A "Web API Key" fica em Configurações do projeto → Geral. O host do banco é
// o que o console mostra em Realtime Database, sem o "https://" e sem a barra
// final. Trocar por chave/host próprios antes de gravar.
static const char* FB_API_KEY = "AIzaSyAZzlDyHvhqszlj8dq2Iy8VuT9uTS3sv9I";
static const char* FB_DB_HOST = "rastreador-gps-c1dc7-default-rtdb.firebaseio.com";
// Cada dispositivo precisa do seu ID. O app usa "rastreador01" por padrão, então
// deixar os dois iguais faria firmware e navegador escreverem na mesma trilha.
static const char* FB_DEVICE  = "esp32-01";

// Intervalo entre envios. Cada envio são duas escritas (última + histórico).
static const uint32_t FB_INTERVAL_MS = 30000;

// false grava só a última posição, sem histórico — o mesmo que o seletor
// "Histórico" da aba Firebase do app.
static const bool FB_TRACK = true;

// De quanto em quanto tempo o device pergunta ao banco se deve estar ligado.
// É o que define a demora entre apertar o botão no app e o device obedecer —
// e, desligado, é praticamente todo o consumo de dados que sobra.
static const uint32_t FB_CONFIG_POLL_MS = 30000;

// Heartbeat em /status: é por ele que o app sabe que o device está vivo.
static const uint32_t FB_STATUS_PUSH_MS = 60000;

// ── Wi-Fi (só para o mapa) ───────────────────────────────────
// Os tiles do OpenStreetMap vêm exclusivamente por aqui e ficam gravados no
// LittleFS. O chip do A7672 é um SIM IoT com franquia pequena, e uma tela de
// mapa são 6 a 12 tiles de ~15 KB — baixar isso por dados móveis não se paga.
// Deixe o aparelho um tempo no Wi-Fi para encher o cache da região; na rua ele
// desenha do flash, sem rede nenhuma.
//
// As credenciais vivem em src/secrets.h, que não é versionado — este repo é
// publicado no GitHub Pages. Sem esse arquivo o firmware compila igual e o
// mapa passa a usar só o cache.
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #define WIFI_SSID_LOCAL ""
  #define WIFI_PASS_LOCAL ""
#endif

static const char* WIFI_SSID = WIFI_SSID_LOCAL;   // vazio = não tenta conectar
static const char* WIFI_PASS = WIFI_PASS_LOCAL;

// Zoom do mapa. 16 mostra ~2 quarteirões na largura da tela.
static const int MAPA_ZOOM = 16;

// ── Log no monitor (Serial0 / USB) ───────────────────────────
// true ecoa todo o tráfego AT. Verboso, mas é o que mostra onde a conversa
// com o módulo trava — deixe ligado até a placa estar validada.
static const bool DEBUG_AT = true;

// Resumo periódico do estado. 0 desliga.
static const uint32_t STATUS_INTERVAL_MS = 30000;

TFT_eSPI   tft;

A7672Core  modem;
A7672Net   net(modem);
A7672Sms   sms(modem);
A7672Voice voice(modem);
A7672Gnss  gnss(modem);
A7672Http  http(modem, net);
A7672Tls   tls(modem, net);
A7672Firebase firebase(tls);

static uint32_t fbEnviados = 0, fbFalhas = 0;

// Último estado da rede. Cada net.refresh() são vários comandos AT disputando
// a UART com o NMEA a 1 Hz; consultar de dois lugares diferentes fazia as
// respostas se cruzarem e voltarem vazias ("0 dBm, sem IP"). Um só ponto
// atualiza, os demais leem daqui.
static NetStatus netCache;
static uint32_t netCacheAt = 0;

static void refreshNet(uint32_t maxAgeMs) {
  if (netCacheAt && millis() - netCacheAt < maxAgeMs) return;
  net.refresh(netCache);
  netCacheAt = millis();
}

// Carimbo de tempo desde o boot: sem ele não dá para saber se duas linhas
// saíram juntas ou com um minuto de intervalo — e é justamente o intervalo
// que denuncia timeout de rede.
static void logf(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  uint32_t ms = millis();
  Serial.printf("[%4lu.%03lu] %s\n", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000), buf);
}

// ── Ponte USB <-> módulo ─────────────────────────────────────
// O A7672 tem uma UART só. Com o ESP32 ligado nela, falar AT pelo PC exigia
// desconectar o ESP32 e plugar um FTDI no módulo. Em modo ponte o ESP32 vira
// um cabo: o que chega do PC vai para o módulo e o que volta sobe para o PC,
// cru. Enquanto isso o firmware para de mandar comandos por conta própria —
// dois mestres na mesma UART fazem as respostas se cruzarem.
static bool ponteAtiva = false;

// Há tela acoplada nesta unidade? Vem da NVS no boot e do /config depois.
// Declarado aqui em cima porque entrarPonte(), logo abaixo, já consulta.
static bool displayAtivo = true;
static String usbLinha;

static void entrarPonte(bool on) {
  ponteAtiva = on;
  modem.setDebug(on ? nullptr : &Serial);   // o eco "<<" atrapalharia o parser do app

  // Em ponte o loop não chega até a tela, e o firmware nem lê mais o NMEA:
  // manter o mapa antigo no display faria parecer travado, com dados velhos.
  // Dizer o que está acontecendo é mais honesto que congelar.
  if (displayAtivo) {
    if (on) ui.splash("MODO PONTE", "o app esta no controle do modulo");
    else    ui.forcarRedesenho();
  }

  Serial.println(on ? "@BRIDGE:ON" : "@BRIDGE:OFF");
}

// Reconexão pedida pelo app. O loop cuida dela; o handler da serial não pode
// esperar associação de Wi-Fi (até 10 s) porque nesse tempo o display congela
// e a própria serial deixa de ser lida — era o que fazia as respostas do
// @WIFI se perderem.
static bool wifiReconectar = false;

// Configuração de Wi-Fi pelo cabo, não pelo banco. A senha de uma rede não
// tem por que subir para a nuvem — ainda mais neste projeto, cuja Web API Key
// é pública por estar no app. Aqui ela vai do navegador direto para a NVS.
//
//   @WIFI?                          -> responde a rede atual (sem a senha)
//   @WIFI {"ssid":"casa","pass":"1234"}  -> grava e reconecta
//   @WIFI!                          -> apaga a rede salva
//
// O corpo vem em JSON para aguentar senha com espaço, ':' ou acento, que um
// separador simples quebraria.
static void comandoWifi(const String& cmd) {
  String ssid, pass;

  if (cmd == "@WIFI?" || cmd == "@WIFI") {
    A7672Firebase::wifiSalvo(ssid, pass);
    Serial.println("@WIFI:" + String(ssid.length() ? "SSID " : "VAZIO ") + ssid
                   + (WiFi.status() == WL_CONNECTED ? " CONECTADO " + WiFi.localIP().toString() : " DESCONECTADO"));
    return;
  }

  if (cmd == "@WIFI!") {
    A7672Firebase::wifiApagar();
    WiFi.disconnect(true);
    Serial.println("@WIFI:APAGADO");
    logf("Wi-Fi: rede salva apagada pelo app.");
    return;
  }

  int chave = cmd.indexOf('{');
  if (chave < 0) { Serial.println("@WIFI:ERRO formato"); return; }
  String json = cmd.substring(chave);
  ssid = A7672Firebase::jsonString(json, "ssid");
  pass = A7672Firebase::jsonString(json, "pass");

  if (!ssid.length()) { Serial.println("@WIFI:ERRO ssid vazio"); return; }
  if (!A7672Firebase::wifiSalvar(ssid, pass)) { Serial.println("@WIFI:ERRO nvs"); return; }

  // Responde já e deixa a conexão para o loop: quem mandou o comando não pode
  // ficar sem resposta enquanto o rádio associa.
  Serial.println("@WIFI:SALVO " + ssid);
  logf("Wi-Fi: rede trocada pelo app para \"%s\" — conectando em segundo plano.", ssid.c_str());
  wifiReconectar = true;
}

// Varredura de redes, disparada pelo app. Assíncrona de propósito: o
// scanNetworks() bloqueante segura o loop por 2 a 4 s, que foi exatamente o
// que congelava o display na versão anterior do @WIFI.
static void comandoScan() {
  int estado = WiFi.scanComplete();
  if (estado == WIFI_SCAN_RUNNING) { Serial.println("@SCAN:JA_RODANDO"); return; }

  WiFi.mode(WIFI_STA);          // sem STA ligado o scan volta vazio
  WiFi.scanDelete();
  WiFi.scanNetworks(true, true);   // async, incluindo redes ocultas
  Serial.println("@SCAN:INICIO");
  logf("Wi-Fi: varrendo redes...");
}

// Publica o resultado quando ele fica pronto. Chamado pelo wifiPump().
static void scanPump() {
  int n = WiFi.scanComplete();
  if (n < 0) return;               // ainda rodando, ou nada pedido

  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    ssid.replace("\\", "\\\\");
    ssid.replace("\"", "\\\"");
    Serial.println("@SCAN:NET {\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i))
                   + ",\"aberta\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}");
  }
  Serial.println("@SCAN:FIM " + String(n));
  logf("Wi-Fi: %d rede(s) encontrada(s).", n);
  WiFi.scanDelete();
}

static void pumpUsb() {
  while (Serial.available()) {
    char c = (char)Serial.read();

    // As linhas de controle começam com '@' e nunca chegam ao módulo.
    if (c == '\n' || c == '\r') {
      String cmd = usbLinha;
      usbLinha = "";
      cmd.trim();
      if (cmd == "@BRIDGE")      { entrarPonte(true);  continue; }
      if (cmd == "@NORMAL")      { entrarPonte(false); continue; }
      if (cmd == "@PING")        { Serial.println(ponteAtiva ? "@BRIDGE:ON" : "@BRIDGE:OFF"); continue; }
      if (cmd.startsWith("@WIFI")) { comandoWifi(cmd); continue; }
      if (cmd == "@SCAN")          { comandoScan();    continue; }
      if (ponteAtiva && cmd.length()) modem.write(cmd + "\r\n");
      continue;
    }

    // Ctrl+Z e ESC terminam SMS e HTTPDATA: precisam passar sem virar linha.
    if (ponteAtiva && (c == 0x1A || c == 0x1B)) {
      if (usbLinha.length()) { modem.write(usbLinha); usbLinha = ""; }
      modem.write(String(c));
      continue;
    }

    if (usbLinha.length() < 512) usbLinha += c;
  }
}

// Atende um pedido de ponte no meio do setup. Sem isto o '@BRIDGE' esperava a
// inicialização inteira — e quem abre a porta pelo Web Serial reinicia o ESP32
// justo antes de pedir, então a espera caía sempre em cima do boot.
static bool ponteSolicitada() {
  pumpUsb();
  if (ponteAtiva) logf("Ponte pedida durante o boot: o firmware nao vai inicializar.");
  return ponteAtiva;
}

// Dispara a associação e volta na hora. Quem acompanha o resultado é o loop,
// em wifiPump() — esperar aqui congelaria display, serial e NMEA.
static void conectarWifi() {
  String ssid, pass;
  bool doApp = A7672Firebase::wifiSalvo(ssid, pass);
  if (!doApp) { ssid = WIFI_SSID; pass = WIFI_PASS; }

  if (!ssid.length()) {
    logf("Wi-Fi: nenhuma rede configurada — mapa so do cache.");
    logf("       Configure na aba ESP32 do app, em 'Wi-Fi do ESP32'.");
    return;
  }

  logf("Wi-Fi: conectando a \"%s\" (%s, so para baixar mapa)...",
       ssid.c_str(), doApp ? "definida pelo app" : "do secrets.h");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
}

// Acompanha a associação sem bloquear e avisa uma única vez quando o estado
// muda. Sem isto não haveria como saber se a rede nova pegou.
static void wifiPump() {
  static wl_status_t anterior = WL_IDLE_STATUS;
  static uint32_t desde = 0;

  scanPump();

  if (wifiReconectar) {
    wifiReconectar = false;
    WiFi.disconnect();
    conectarWifi();
    desde = millis();
    anterior = WL_IDLE_STATUS;
    return;
  }

  wl_status_t agora = WiFi.status();
  if (agora == anterior) {
    // Sem associar em 20 s costuma ser senha errada ou rede fora de alcance.
    if (agora != WL_CONNECTED && desde && millis() - desde > 20000) {
      desde = 0;
      logf("Wi-Fi: nao conectou (senha errada ou rede fora de alcance?) — mapa so do cache.");
      Serial.println("@WIFI:FALHA nao conectou");
    }
    return;
  }

  anterior = agora;
  if (agora == WL_CONNECTED) {
    desde = 0;
    logf("Wi-Fi: conectado — %s", WiFi.localIP().toString().c_str());
    Serial.println("@WIFI:OK " + WiFi.localIP().toString());
  }
}

static void printFix(const GnssFix& f) {
  if (!f.valid) {
    logf("GNSS: sem fix ainda (TTFF a frio leva 30-90 s com vista para o ceu)");
    return;
  }
  logf("GNSS: %.6f, %.6f | fix %dD | alt %.0f m | %d sat | HDOP %.2f | %s %s UTC",
       f.lat, f.lon, f.mode, f.altitude, f.svTotal, f.hdop,
       f.utcDate.c_str(), f.utcTime.c_str());
  logf("      GPS %d | GLONASS %d | Galileo %d | BeiDou %d",
       f.svGps, f.svGlonass, f.svGalileo, f.svBeidou);
}

// Uma linha com tudo que importa para saber se o conjunto está de pé.
static void printStatus() {
  const GnssFix& f = gnss.fix();
  String gnssTxt = f.valid
      ? String("fix ") + f.mode + "D, " + f.svTotal + " sat"
      : String("sem fix");

  if (modem.sim() == SimState::Ready) {
    refreshNet(45000);   // aproveita a leitura do heartbeat quando é recente
    logf("STATUS | rede: %s %s %d dBm IP %s | GNSS: %s | envio %s | Firebase: %lu env, %lu falhas",
         netCache.operatorName.c_str(), netCache.tech.c_str(), netCache.dbm,
         netCache.ip.length() ? netCache.ip.c_str() : "sem IP",
         gnssTxt.c_str(), firebase.enabled() ? "ATIVO" : "parado",
         (unsigned long)fbEnviados, (unsigned long)fbFalhas);
  } else {
    logf("STATUS | SIM: %s | GNSS: %s", A7672Core::simText(modem.sim()), gnssTxt.c_str());
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  logf("=== A7672SA / ESP32 ===");

  // Desligado por padrão: a maioria das unidades não leva tela, e ligar o que
  // não existe custaria SPI, cache de mapa e tempo de loop à toa. Quem tem
  // display ativa uma vez pelo app e a NVS lembra — a decisão precisa estar
  // tomada antes de haver rede para ler o banco.
  displayAtivo = A7672Firebase::displaySalvo(false);

  if (displayAtivo) {
    ui.begin();
    ui.splash("Rastreador GPS", "iniciando...");
    if (mapa.begin()) logf("Cache de mapa: %u tiles no LittleFS.", (unsigned)mapa.tilesEmCache());
    else              logf("Cache de mapa indisponivel: %s", mapa.ultimoErro().c_str());
  } else {
    logf("Display DESATIVADO (padrao). Nem SPI, nem mapa, nem tiles.");
    logf("       Tem tela nesta unidade? Ative na aba ESP32 do app.");
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, LOW);   // backlight apagado consome de verdade menos
  }

  // Wi-Fi serve só para encher o cache de tiles; nada do rastreamento depende
  // dele. A rede configurada pelo app (guardada na NVS) tem precedência sobre
  // o secrets.h, que é só o padrão de fábrica.
  conectarWifi();
  logf("UART2 @ 115200 8N1 — RX=GPIO%d TX=GPIO%d | PWRKEY=GPIO%d (ativo em %s) | STATUS=%s",
       PIN_RX, PIN_TX, PIN_PWRKEY, PWRKEY_ACTIVE_HIGH ? "ALTO" : "BAIXO",
       PIN_STATUS >= 0 ? String(String("GPIO") + PIN_STATUS).c_str() : "nao usado");
  logf("APN: %s | Firebase: %s (device %s, a cada %lu s)",
       APN, FB_DB_HOST, FB_DEVICE, (unsigned long)(FB_INTERVAL_MS / 1000));

  modem.begin(Serial2, PIN_RX, PIN_TX, 115200);
  if (DEBUG_AT) modem.setDebug(&Serial);

  modem.setStatusPin(PIN_STATUS);

  // Nada de inicializar no escuro: sem o módulo ligado e respondendo, toda a
  // sequência abaixo iria para o vazio e ainda pareceria ter dado certo.
  logf("Ligando o modulo (pulso no PWRKEY, a UART leva ~8 s para subir)...");
  if (ponteSolicitada()) return;
  if (!modem.ensurePowered(PIN_PWRKEY, PWRKEY_ACTIVE_HIGH)) {
    logf("FALHA: modulo sem resposta — inicializacao ABORTADA.");
    logf("  Verifique nesta ordem:");
    logf("  1) alimentacao 3,4-4,2 V aguentando pico de 2 A");
    logf("  2) PWRKEY: com ligacao direta no modulo, PWRKEY_ACTIVE_HIGH deve ser false");
    logf("  3) TX/RX cruzados (GPIO%d no TX do modulo, GPIO%d no RX)", PIN_RX, PIN_TX);
    logf("  4) GND comum entre ESP32 e modulo");
    logf("  5) STATUS: se o pino nao esta ligado, use PIN_STATUS = -1");
    return;
  }
  logf("Modulo respondeu ao AT.");
  if (ponteSolicitada()) return;

  modem.at("ATE0");        // eco atrapalha o parser
  modem.at("AT+CMEE=2");   // erros por extenso

  // ── SIM ────────────────────────────────────────────────────
  modem.refreshSim();
  logf("SIM: %s", A7672Core::simText(modem.sim()));
  if (ponteSolicitada()) return;

  // ── GNSS: independe de SIM, então roda de qualquer jeito ───
  if (gnss.powerOn(3)) {
    gnss.startNmea(1);
    logf("GNSS ligado, stream NMEA a 1 Hz.");
  } else {
    logf("GNSS indisponivel: %s", modem.lastError().c_str());
  }

  // Daqui para baixo tudo exige SIM. Os módulos já barram sozinhos, mas sair
  // cedo evita esperar timeouts inúteis.
  if (modem.sim() != SimState::Ready) {
    logf("Sem SIM: rede, SMS, voz, HTTP e Firebase ficam desabilitados.");
    logf("O GNSS continua funcionando — os fixes aparecem abaixo, mas nao sobem.");
    return;
  }

  if (ponteSolicitada()) return;

  // ── Rede ───────────────────────────────────────────────────
  logf("Registrando na rede (ate 60 s)...");
  net.setApn(APN);
  net.attach();

  if (!net.waitRegistered(60000)) logf("AVISO: nao registrou na rede em 60 s.");

  NetStatus st;
  net.refresh(st);
  logf("Rede: %s | %s | %d dBm | RSRP %d | IP %s",
       st.operatorName.c_str(), st.tech.c_str(), st.dbm, st.rsrp,
       st.ip.length() ? st.ip.c_str() : "sem IP (contexto PDP nao subiu)");

  // ── SMS ────────────────────────────────────────────────────
  sms.begin();
  sms.onArrived([](int index, const String& storage) {
    logf("SMS novo em %s[%d]", storage.c_str(), index);
    SmsMessage m;
    if (sms.read(index, m)) logf("  de %s: %s", m.sender.c_str(), m.text.c_str());
  });

  // ── Voz ────────────────────────────────────────────────────
  voice.begin();
  voice.onEvent([](CallState s, const String& number) {
    const char* nomes[] = { "ociosa", "discando", "tocando", "ativa", "ocupado", "sem resposta" };
    logf("Chamada: %s (%s)", nomes[(int)s], number.c_str());
  });

  // ── HTTP ───────────────────────────────────────────────────
  // httpbin.org devolve 503 com frequência, e no log isso parece falha do
  // módulo quando é só o serviço de teste fora do ar.
  logf("Testando HTTPS (AT+HTTP*)...");
  HttpResponse r = http.get("https://postman-echo.com/get");
  if (r.status == -1)      logf("HTTP: sem contexto PDP — confira o APN.");
  else if (r.status == 715) logf("HTTP: erro 715 = handshake TLS. Falta AT+CSSLCFG=\"enableSNI\",0,1.");
  else                      logf("HTTP %d, %u bytes.", r.status, (unsigned)r.length);

  // ── Firebase ───────────────────────────────────────────────
  logf("Autenticando no Firebase...");
  firebase.begin(FB_API_KEY, FB_DB_HOST, FB_DEVICE);
  firebase.setTrackEnabled(FB_TRACK);   // o /config do app pode sobrescrever
  if (firebase.ensureAuth()) {
    logf("Firebase: autenticado (usuario anonimo).");

    // O rastreamento nasce desligado: quem manda ligar é o app, escrevendo em
    // /devices/<id>/config. Sem isso o device gastaria dados sem ninguem pedir.
    if (firebase.fetchConfig())
      logf("Config lido: envio %s.", firebase.enabled() ? "ATIVADO pelo app" : "DESATIVADO");
    else
      logf("Config nao lido (%s) — envio segue desativado.", firebase.lastError().c_str());

    firebase.remoteLog("info", String("Boot. Rede ") + st.tech + " " + String(st.dbm) + " dBm, IP " + st.ip);
  } else {
    logf("Firebase FALHOU: %s", firebase.lastError().c_str());
  }

  logf("=== inicializacao concluida ===");
  logf("Envio %s. Use o botao 'Ativar envio' na aba Firebase do app para mudar.",
       firebase.enabled() ? "ATIVO" : "PARADO (aguardando comando do app)");
  logf("Ponte: mande '@BRIDGE' por esta serial para falar AT com o modulo "
       "atraves do ESP32, e '@NORMAL' para devolver o controle ao firmware.");
  logf("Wi-Fi: '@WIFI?' mostra a rede, '@WIFI {\"ssid\":\"..\",\"pass\":\"..\"}' troca, "
       "'@WIFI!' apaga, '@SCAN' lista as redes. A senha nunca passa pelo Firebase.");
}

void loop() {
  pumpUsb();
  wifiPump();

  // Em modo ponte o firmware sai de cena: repassa os bytes crus e não toca em
  // nada. Deixar o pump() rodar aqui comeria as respostas antes de chegarem ao
  // PC, e o envio periódico disputaria a UART com quem está do outro lado.
  if (ponteAtiva) {
    while (Serial2.available()) Serial.write((char)Serial2.read());

    // Um sinal de vida piscando devagar: sem isto a tela de ponte fica
    // estática e o aparelho continua parecendo travado.
    static uint32_t pisca = 0;
    static bool aceso = false;
    if (displayAtivo && millis() - pisca > 1200) {
      pisca = millis();
      aceso = !aceso;
      tft.fillCircle(462, 302, 5, aceso ? 0x3D7F : 0x0861);
    }
    return;
  }

  // Sem comando em andamento, é aqui que as URCs (NMEA, +CMTI, RING) são lidas.
  modem.pump(50);

  static uint32_t last = 0;
  if (millis() - last > 10000) {
    last = millis();
    printFix(gnss.fix());
  }

  // ── Ordens vindas do app ────────────────────────────────────
  // Continua consultando mesmo desativado: é assim que o device fica sabendo
  // que foi religado.
  static uint32_t lastConfig = 0;
  if (firebase.authenticated() && millis() - lastConfig > FB_CONFIG_POLL_MS) {
    lastConfig = millis();
    bool antes = firebase.enabled();
    if (!firebase.fetchConfig()) {
      // Silenciar aqui esconde o motivo de o device ignorar o botão do app.
      logf("Config: falha ao ler — %s", firebase.lastError().c_str());
    } else if (firebase.enabled() != antes) {
      logf("COMANDO do app: envio %s.", firebase.enabled() ? "ATIVADO" : "DESATIVADO");
      firebase.remoteLog("info", firebase.enabled() ? "Envio ativado pelo app"
                                                    : "Envio desativado pelo app");
    }

    // Display ligado/desligado pelo app, sem precisar reiniciar.
    if (firebase.displayLigado() != displayAtivo) {
      displayAtivo = firebase.displayLigado();
      logf("COMANDO do app: display %s.", displayAtivo ? "ATIVADO" : "DESATIVADO");
      firebase.remoteLog("info", displayAtivo ? "Display ativado" : "Display desativado");
      if (displayAtivo) {
        ui.begin();
        mapa.begin();
        ui.forcarRedesenho();
      } else {
        // Só parar de desenhar deixaria a última tela acesa para sempre.
        tft.fillScreen(0x0000);
        pinMode(TFT_BL, OUTPUT);
        digitalWrite(TFT_BL, LOW);
      }
    }
  }

  // ── Envio periódico ─────────────────────────────────────────
  // Só sobe com fix válido: mandar 0,0 sujaria o histórico com pontos no golfo
  // da Guiné. E só com autorização do app.
  uint32_t intervalo = firebase.remoteInterval() ? firebase.remoteInterval() * 1000UL
                                                 : FB_INTERVAL_MS;
  static uint32_t lastSend = 0;
  if (firebase.enabled() && millis() - lastSend > intervalo) {
    lastSend = millis();
    const GnssFix& f = gnss.fix();
    if (!f.valid) {
      // Dizer que está esperando evita a leitura de que o envio travou.
      logf("Firebase: aguardando fix — nada enviado.");
    } else if (firebase.sendFix(f)) {
      fbEnviados++;
      logf("Firebase: OK  %.6f, %.6f -> /devices/%s (total %lu)",
           f.lat, f.lon, FB_DEVICE, (unsigned long)fbEnviados);
    } else if (firebase.lastError().startsWith("Parado")) {
      // Filtrado por distância é o filtro trabalhando, não erro: não conta
      // como falha nem gasta uma escrita de log no banco.
      logf("Firebase: %s", firebase.lastError().c_str());
    } else {
      fbFalhas++;
      logf("Firebase: FALHA (%lu) — %s", (unsigned long)fbFalhas, firebase.lastError().c_str());
      // Falha vai para o banco: é o que o app tem para diagnosticar de longe.
      firebase.remoteLog("erro", firebase.lastError());
    }
  }

  // ── Heartbeat ───────────────────────────────────────────────
  uint32_t statusMs = firebase.statusEvery() ? firebase.statusEvery() * 1000UL
                                             : FB_STATUS_PUSH_MS;
  static uint32_t lastPush = 0;
  if (firebase.authenticated() && millis() - lastPush > statusMs) {
    lastPush = millis();
    const GnssFix& f = gnss.fix();
    refreshNet(30000);
    firebase.sendStatus(A7672Firebase::isoTimestamp(f.utcDate, f.utcTime),
                        f.valid, f.svTotal, netCache.dbm, fbEnviados, fbFalhas);
  }

  static uint32_t lastStatus = 0;
  if (STATUS_INTERVAL_MS && millis() - lastStatus > STATUS_INTERVAL_MS) {
    lastStatus = millis();
    printStatus();
  }

  // ── Tela ────────────────────────────────────────────────────
  // Sem display, nem o UiEstado é montado: ele constrói Strings a cada meio
  // segundo e não haveria para quem mostrar.
  static uint32_t lastUi = 0;
  if (displayAtivo && millis() - lastUi > 500) {
    lastUi = millis();
    const GnssFix& f = gnss.fix();
    UiEstado e;
    e.fix = f.valid; e.mode = f.mode;
    e.lat = f.lat; e.lon = f.lon; e.alt = f.altitude;
    e.kmh = f.speedKmh; e.hdop = f.hdop; e.sats = f.svTotal;
    // Hora local na tela: quem olha o aparelho quer saber que horas são aqui,
    // não em Greenwich. O que sobe para o banco continua em UTC.
    e.utc = A7672Firebase::localTimestamp(f.utcDate, f.utcTime, firebase.tzMinutes());
    e.operadora = netCache.operatorName; e.tech = netCache.tech;
    e.dbm = netCache.dbm; e.online = netCache.ip.length() > 0;
    e.envioAtivo = firebase.enabled();
    e.enviados = fbEnviados; e.falhas = fbFalhas;
    e.wifi = (WiFi.status() == WL_CONNECTED);
    e.tilesCache = mapa.tilesEmCache();
    ui.atualizar(e);
  }

  // Enche o cache de tiles enquanto houver Wi-Fi. UM por vez: cada download é
  // uma requisição HTTPS que segura o loop por 1 a 3 s, e nesse tempo o
  // display não atualiza e o NMEA se acumula. Encher o cache é o trabalho
  // menos urgente que este firmware tem.
  // Tiles existem só para o mapa: sem tela, baixar seria gastar Wi-Fi, flash
  // e tempo de loop para nada.
  static uint32_t lastTile = 0;
  if (displayAtivo && WiFi.status() == WL_CONNECTED && gnss.fix().valid
      && millis() - lastTile > 5000) {
    lastTile = millis();
    const GnssFix& f = gnss.fix();
    if (mapa.precarregar(f.lat, f.lon, MAPA_ZOOM, 1, 1) > 0) ui.marcarMapaSujo();
  }
}
