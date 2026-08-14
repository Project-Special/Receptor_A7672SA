# A7672SA Controller

PWA de bancada para o módulo **SIMCom A7672SA** (LTE Cat.1 bis + GSM + GNSS),
usando o conjunto de comandos **A76XX**. Página única, sem build, sem servidor:
abra `index.html` no Chrome/Edge e conecte na porta serial do módulo.

Derivado do projeto irmão `Receptor_SIM808`, com o conjunto de comandos e os
painéis reescritos para os recursos do A7672SA.

---

## Como usar

1. Ligue o A7672SA no PC (USB "AT Interface" ou conversor USB-UART em **115200 8N1**).
2. Abra `index.html` no Chrome ou Edge (Web Serial exige contexto `file://` ou HTTPS).
3. `🔍 Detectar Portas` → escolha a porta → **Conectar**.
   Com *Auto-init* marcado, a interface roda a sequência de identificação sozinha.
4. Sem hardware por perto? Escolha **Simulador (sem hardware)** em Interface e
   clique em Conectar: o simulador embutido responde a todos os comandos com dados
   sintéticos, inclusive posição GNSS em movimento, tráfego MQTT e respostas HTTP.

### Três formas de conexão

| Modo | Quando usar |
|---|---|
| **Serial (USB/UART)** | Chrome/Edge no desktop — caminho normal |
| **WebSocket Bridge** | navegadores sem Web Serial (Firefox, Safari, Android) ou módulo em outra máquina |
| **Simulador** | sem hardware: responde a tudo com dados sintéticos, nada é transmitido |

Enquanto não houver conexão ativa, os botões de comando ficam desabilitados e
nenhum byte é enviado — inclusive as macros e o campo de comando manual.

Para o bridge:

```bash
pip install websockets pyserial
python a7672_bridge.py                 # escuta em ws://0.0.0.0:8765
python a7672_bridge.py --port COM7     # fixa a porta
```

Na interface: *Interface → WebSocket Bridge*, informe a URL e conecte.
O botão de detecção lista as portas seriais vistas pelo bridge.

---

## Painéis

| Aba | O que faz |
|---|---|
| **Terminal** | console AT com histórico (↑/↓), autocomplete (Tab), Ctrl+Z/ESC, export do log |
| **GNSS** | `+CGNSSINFO` decodificado campo a campo, view bruta, stream NMEA, poll configurável, constelações |
| **Rede** | RSSI/RSRP/RSRQ/SINR com medidores, célula servidora via `AT+CPSI?`, registro, APN, preferência de rede, varredura de operadoras |
| **Mapa** | Leaflet + OSM, rastro, seguir, distância acumulada, export GPX |
| **SMS** | envio GSM/UCS2 com contagem de partes, listagem, leitura por índice, URCs |
| **Voz** | discagem, atender/desligar, duração, DTMF, volume e canal de áudio |
| **HTTP** | GET/POST/HEAD/DELETE com corpo via `AT+HTTPDATA`, cabeçalhos, TLS, leitura paginada |
| **MQTT** | sequência completa START→ACCQ→CONNECT→PUB/SUB, TLS, publicação da posição GNSS |
| **TCP/IP** | `NETOPEN`/`CIPOPEN`/`CIPSEND` em TCP ou UDP, contador de bytes, log do socket |
| **Arquivos/SSL** | filesystem `C:/`, upload de certificado PEM via `AT+CCERTDOWN`, contextos SSL |
| **Hardware** | tensão, temperatura, ADC, GPIO, relógio, NTP, modos de energia |
| **Info** | identidade do módulo, diagnóstico copiável, quadro de recursos do chip |

---

## Arquivos

```
index.html         aplicação inteira (UI + lógica + simulador)
manifest.json      PWA
sw.js              service worker (cache offline; tiles do mapa em network-first)
icon.svg           ícone
a7672_bridge.py    bridge WebSocket ↔ serial (pyserial)
a7672sa.md         referência dos comandos AT e dos recursos do chip
```

---

## Notas sobre o hardware

- **Alimentação**: 3.4–4.2 V com capacidade de pico ~2 A. Fonte fraca causa reset
  justamente durante o registro na rede — é o problema mais comum na bancada.
- **Sem Bluetooth e sem Wi-Fi**: o A7672SA não tem esses rádios. Os comandos
  `AT+BT*` do SIM808 não existem aqui.
- **GNSS só nas variantes `-FASE`**: as variantes cujo sufixo começa com `L` não
  trazem o receptor GNSS.
- **Baud padrão 115200** (o SIM808 usava 9600).
- Nomes que mudaram em relação ao SIM808: `CGNSPWR`→`CGNSSPWR`, `CGNSINF`→`CGNSSINFO`,
  `CCID`→`CICCID`, `HTTPREAD`→`HTTPREAD=<offset>,<len>`.

Detalhes de cada comando em [`a7672sa.md`](a7672sa.md).
