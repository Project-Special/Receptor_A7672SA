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

// ── Log no monitor (Serial0 / USB) ───────────────────────────
// true ecoa todo o tráfego AT. Verboso, mas é o que mostra onde a conversa
// com o módulo trava — deixe ligado até a placa estar validada.
static const bool DEBUG_AT = true;

// Resumo periódico do estado. 0 desliga.
static const uint32_t STATUS_INTERVAL_MS = 30000;

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

  modem.at("ATE0");        // eco atrapalha o parser
  modem.at("AT+CMEE=2");   // erros por extenso

  // ── SIM ────────────────────────────────────────────────────
  modem.refreshSim();
  logf("SIM: %s", A7672Core::simText(modem.sim()));

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
}

void loop() {
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
}
