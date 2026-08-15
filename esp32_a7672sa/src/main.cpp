// Demonstração do A7672SA no ESP32.
// Cada subsistema vive em lib/<Modulo>/ e recebe o núcleo por referência.
#include <Arduino.h>
#include <core.h>
#include <net.h>
#include <sms.h>
#include <voice.h>
#include <gnss.h>
#include <http.h>

// ── Pinos ────────────────────────────────────────────────────
// UART2 do ESP32 ligada à UART principal do módulo (115200 8N1).
static const int PIN_RX     = 16;   // ESP32 RX  <- TX do módulo
static const int PIN_TX     = 17;   // ESP32 TX  -> RX do módulo
static const int PIN_PWRKEY = 4;
static const int PIN_STATUS = 5;    // -1 se o STATUS do módulo não estiver ligado

// A placa Muz 24x24 inverte o PWRKEY com um transistor: o header é ativo em
// ALTO. Ligando direto no pino do módulo, troque para false (ativo em BAIXO).
static const bool PWRKEY_ACTIVE_HIGH = true;

static const char* APN = "claro.com.br";

A7672Core  modem;
A7672Net   net(modem);
A7672Sms   sms(modem);
A7672Voice voice(modem);
A7672Gnss  gnss(modem);
A7672Http  http(modem, net);

static void printFix(const GnssFix& f) {
  if (!f.valid) { Serial.println(F("GNSS: sem fix ainda")); return; }
  Serial.printf("GNSS: %.6f, %.6f  alt %.0f m  %d sat  HDOP %.2f\n",
                f.lat, f.lon, f.altitude, f.svTotal, f.hdop);
  Serial.printf("      GPS %d | GLONASS %d | Galileo %d | BeiDou %d\n",
                f.svGps, f.svGlonass, f.svGalileo, f.svBeidou);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("\n=== A7672SA / ESP32 ==="));

  modem.begin(Serial2, PIN_RX, PIN_TX, 115200);
  modem.setDebug(&Serial);

  modem.setStatusPin(PIN_STATUS);

  // Nada de inicializar no escuro: sem o módulo ligado e respondendo, toda a
  // sequência abaixo iria para o vazio e ainda pareceria ter dado certo.
  if (!modem.ensurePowered(PIN_PWRKEY, PWRKEY_ACTIVE_HIGH)) {
    Serial.println(F("Modulo sem resposta — inicializacao ABORTADA."));
    Serial.println(F("Verifique: alimentacao 3,4-4,2 V com pico de 2 A, PWRKEY, TX/RX e GND comum."));
    return;
  }

  modem.at("ATE0");        // eco atrapalha o parser
  modem.at("AT+CMEE=2");   // erros por extenso

  // ── SIM ────────────────────────────────────────────────────
  modem.refreshSim();
  Serial.printf("SIM: %s\n", A7672Core::simText(modem.sim()));

  // ── GNSS: independe de SIM, então roda de qualquer jeito ───
  if (gnss.powerOn(3)) {
    gnss.startNmea(1);
    Serial.println(F("GNSS ligado. TTFF a frio: 30-90 s com vista para o ceu."));
  } else {
    Serial.printf("GNSS indisponivel: %s\n", modem.lastError().c_str());
  }

  // Daqui para baixo tudo exige SIM. Os módulos já barram sozinhos, mas sair
  // cedo evita esperar timeouts inúteis.
  if (modem.sim() != SimState::Ready) {
    Serial.println(F("Sem SIM: rede, SMS, voz e HTTP ficam desabilitados."));
    return;
  }

  // ── Rede ───────────────────────────────────────────────────
  net.setApn(APN);
  net.attach();

  if (!net.waitRegistered(60000)) Serial.println(F("Nao registrou na rede."));

  NetStatus st;
  net.refresh(st);
  Serial.printf("Rede: %s | %s | %d dBm | RSRP %d | IP %s\n",
                st.operatorName.c_str(), st.tech.c_str(), st.dbm, st.rsrp, st.ip.c_str());

  // ── SMS ────────────────────────────────────────────────────
  sms.begin();
  sms.onArrived([](int index, const String& storage) {
    Serial.printf("SMS novo em %s[%d]\n", storage.c_str(), index);
    SmsMessage m;
    if (sms.read(index, m))
      Serial.printf("  de %s: %s\n", m.sender.c_str(), m.text.c_str());
  });

  // ── Voz ────────────────────────────────────────────────────
  voice.begin();
  voice.onEvent([](CallState s, const String& number) {
    const char* nomes[] = { "ociosa", "discando", "tocando", "ativa", "ocupado", "sem resposta" };
    Serial.printf("Chamada: %s (%s)\n", nomes[(int)s], number.c_str());
  });

  // ── HTTP ───────────────────────────────────────────────────
  HttpResponse r = http.get("https://httpbin.org/get");
  if (r.status == -1) Serial.println(F("HTTP: sem contexto PDP — confira o APN."));
  else Serial.printf("HTTP %d, %u bytes:\n%s\n", r.status, (unsigned)r.length, r.body.c_str());
}

void loop() {
  // Sem comando em andamento, é aqui que as URCs (NMEA, +CMTI, RING) são lidas.
  modem.pump(50);

  static uint32_t last = 0;
  if (millis() - last > 10000) {
    last = millis();
    printFix(gnss.fix());
  }
}
