# A7672SA no ESP32

Porte em C++ da interface web deste repositório. Cada subsistema é uma
biblioteca independente em `lib/`, recebendo o núcleo por referência.

```
esp32_a7672sa/
├── platformio.ini
├── src/main.cpp          demonstração de uso
└── lib/
    ├── Core/   core.cpp    UART, motor AT, URCs, PWRKEY, estado do SIM
    ├── Net/    net.cpp     registro, sinal, APN, contexto PDP
    ├── Sms/    sms.cpp     envio/leitura, GSM e UCS2, URC +CMTI
    ├── Voice/  voice.cpp   discagem, atendimento, DTMF, eventos
    ├── Gnss/   gnss.cpp    +CGNSSINFO e parser NMEA
    └── Http/   http.cpp    GET/POST com leitura paginada
```

## Ligação

| ESP32 | Módulo | Observação |
|---|---|---|
| GPIO16 (RX) | TX | |
| GPIO17 (TX) | RX | |
| GPIO4 | PWRK | ativo em ALTO na placa Muz 24x24 |
| GND | GND | terra comum é obrigatório |

Alimentação do módulo: 3,4–4,2 V suportando **picos de 2 A** em TX. Fonte fraca
reinicia o módulo exatamente durante o registro na rede.

## Uso

```cpp
modem.begin(Serial2, 16, 17, 115200);
modem.powerOn(4, /*activeHigh=*/true);   // pulso de ~50 ms
modem.waitReady(20000);                  // UART responde ~8 s depois

modem.refreshSim();
if (modem.sim() == SimState::Ready) {
  net.setApn("claro.com.br");
  net.attach();
  HttpResponse r = http.get("https://exemplo.com/api");
}
```

No `loop()`, chame `modem.pump()`. É ali que URCs assíncronas (NMEA, `+CMTI`,
`RING`) são lidas — sem isso elas só aparecem na próxima resposta de comando.

## Decisões que vieram de bugs reais

Cada uma destas custou uma sessão de depuração na versão web:

**Espera da URC `+CGNSSPWR: READY!`** — configurar constelações logo após o
`AT+CGNSSPWR=1` faz o módulo devolver ERROR. Um `delay()` fixo não serve; o
tempo varia. `A7672Gnss::powerOn()` aguarda a URC.

**Leitura HTTP por contagem de bytes** — encerrar o corpo no primeiro `OK`
trunca respostas que contenham `OK` numa linha, e quebra de vez quando outro
comando responde em paralelo. O `+HTTPREAD: <n>` anuncia o tamanho exato;
`A7672Http::readBody()` conta bytes.

**Leitura paginada em blocos de 1 KB** — pedir 50 KB de uma vez estoura o
buffer da UART antes do ESP32 drenar.

**Contexto PDP verificado antes do HTTP** — sem ele o `AT+HTTPACTION` só falha
após dezenas de segundos, sem indicar que o problema é rede. `dataReady()`
resolve em ~1 s.

**Satélites por constelação vindos das GSA** — o `+CGNSSINFO` não reporta
Galileo. As GSA trazem os satélites em uso nos campos 3..14, uma por
constelação, identificada pelo *system id* do último campo.

**`requireSim()` bloqueando só com certeza** — no estado `Unknown` tudo passa,
senão a primeira consulta seria impossível. Diagnóstico (CSQ, CPSI, CEREG) e
GNSS nunca são bloqueados: é o que se quer usar quando o SIM é o problema.

**`ATD` com `;` final** — sem o ponto-e-vírgula o módulo tenta uma chamada de
dados e a ligação de voz falha.

**Charset restaurado após SMS em UCS2** — `AT+CSCS` é global; deixá-lo em UCS2
quebra as leituras seguintes.

## Build

```bash
pio run              # compila
pio run -t upload    # grava
pio device monitor   # 115200
```

Testado com `platform = espressif32`, `board = esp32dev`: 20 KB de RAM e
334 KB de flash.
