# Firebase — rastreamento GPS do A7672SA

Guia para pôr latitude, longitude e hora do GNSS no Realtime Database, usando
usuário anônimo do Firebase Auth.

O firmware correspondente está em `esp32_a7672sa/lib/Firebase/` e usa o cliente
TLS de `esp32_a7672sa/lib/Tls/`.

---

## 1. Por que não dá para usar o `AT+HTTP*` daqui

Medido neste módulo (fw `A7672M7_B19V01_250905`):

| Parâmetro | Limite aceito |
|---|---|
| `AT+HTTPPARA="URL",...` | **600** caracteres |
| `AT+HTTPPARA="USERDATA",...` (headers) | **287** caracteres |

Um `idToken` do Firebase Auth tem cerca de **900** caracteres. Ele não cabe nem
na query string (`?auth=`) nem no header `Authorization`. Por isso o envio vai
pelo canal SSL cru (`AT+CCHOPEN` / `AT+CCHSEND`), onde o request HTTP é montado
byte a byte e não existe esse teto.

Consequência prática: se um dia trocar a autenticação por um *database secret*
(~40 caracteres), o cliente de `lib/Http/` volta a servir e `lib/Tls/` fica
opcional.

---

## 2. Criar o projeto

O CLI já está instalado (`firebase --version` → 15.x). Login e criação:

```bash
firebase login                       # abre o navegador
firebase projects:create meu-rastreador
firebase use meu-rastreador          # grava o projeto no .firebaserc
```

O resto **exige o console** — não há comando de CLI que ligue Realtime Database
nem provedores de autenticação:

1. **Realtime Database** → <https://console.firebase.google.com> → Build →
   Realtime Database → *Criar banco de dados* → região `us-central1` →
   começar em **modo bloqueado** (as regras da seção 4 abrem o necessário).
2. **Authentication** → Build → Authentication → *Começar* → aba
   **Sign-in method** → habilitar **Anônimo**.

---

## 3. Pegar as duas credenciais

| O que | Onde |
|---|---|
| **Web API Key** | ⚙️ Configurações do projeto → Geral → *Chave de API da Web* |
| **Host do banco** | Realtime Database → a URL no topo, ex. `meu-rastreador-default-rtdb.firebaseio.com` |

O host vai **sem** `https://` e **sem** barra final.

A Web API Key não é segredo — ela identifica o projeto, não autoriza nada
sozinha. Quem autoriza são as regras da próxima seção.

---

## 4. Regras de segurança

As regras já estão versionadas em [database.rules.json](database.rules.json), e
[firebase.json](firebase.json) aponta para elas. Depois do `firebase use`:

```bash
firebase deploy --only database
```

Isso evita colar JSON no console e deixa a mudança no git. O conteúdo é este:

```json
{
  "rules": {
    "devices": {
      "$device": {
        ".read":  "auth != null",
        ".write": "auth != null",
        "last": {
          ".validate": "newData.hasChildren(['lat','lon'])"
        },
        "track": {
          "$day": {
            "$entry": {
              ".validate": "newData.hasChildren(['lat','lon'])",
              "lat": { ".validate": "newData.isNumber() && newData.val() >= -90  && newData.val() <= 90" },
              "lon": { ".validate": "newData.isNumber() && newData.val() >= -180 && newData.val() <= 180" }
            }
          }
        }
      }
    }
  }
}
```

**Limitação que vale saber:** qualquer usuário anônimo autenticado pode escrever
no caminho de qualquer device. Para uma frota própria isso costuma bastar. Se
precisar isolar de verdade, troque o caminho por `/devices/$uid` e a regra por
`"$uid": { ".write": "auth.uid === $uid" }` — o `localId` devolvido no signUp é
esse UID, e aí o `deviceId` legível vira só um campo dentro do nó.

---

## 4b. Conferir antes de gravar o firmware

[tools/fb_check.py](tools/fb_check.py) faz do PC exatamente o que o módulo fará
— usuário anônimo, escrita da última posição, ponto no nó do dia, leitura de
volta — e ainda testa se as regras recusam dado inválido:

```bash
python tools/fb_check.py --api-key AIza... --db-host meu-rastreador-default-rtdb.firebaseio.com
```

Cada falha aponta o passo pendente:

| Mensagem | O que falta |
|---|---|
| `provedor Anonimo desabilitado` | seção 2, item 2 |
| `Web API Key invalida` | seção 3 |
| `401 ... regras ainda bloqueadas` | `firebase deploy --only database` |
| `404 — host do banco errado` | conferir a URL do RTDB |

Rodar isso antes de gravar separa problema de nuvem de problema de firmware —
que foi o que custou caro no diagnóstico do SMS.

---

## 5. Configurar o firmware

Em [main.cpp](esp32_a7672sa/src/main.cpp), no topo:

```cpp
static const char* FB_API_KEY = "AIza...";                                  // seção 3
static const char* FB_DB_HOST = "meu-rastreador-default-rtdb.firebaseio.com";
static const char* FB_DEVICE  = "rastreador01";
static const uint32_t FB_INTERVAL_MS = 30000;
```

Ajuste também o `APN` para o do seu chip — o do protótipo é um SIM emnify
(`em.mnc051.mcc724.gprs`), não `claro.com.br`.

Compilar e gravar:

```bash
pio run -t upload && pio device monitor
```

---

## 6. O que chega no banco

```
/devices/rastreador01/last                     -> sobrescrito a cada envio
/devices/rastreador01/track/2026-08-18/<auto>  -> histórico do dia
/devices/rastreador01/track/2026-08-19/<auto>  -> o dia seguinte abre outro nó
```

**Um nó por dia.** A chave do dia sai da própria data do satélite (os 10
primeiros caracteres do `utc`), então a virada acontece à meia-noite **UTC** —
que no horário de Brasília é 21h. Se o corte precisar ser à meia-noite local,
subtraia 3 h antes de formar a chave; hoje o firmware não faz isso porque o
único relógio confiável a bordo é o do GNSS, que é UTC por definição.

Enquanto o receptor ainda não entregou a data (acontece nos primeiros quadros
NMEA, quando já há posição mas o RMC não chegou), os pontos vão para
`track/sem-data/` em vez de se perderem ou entrarem num dia errado.

Isso torna barato ler ou apagar um dia inteiro:

```bash
# baixar um dia
curl "https://<host>/devices/rastreador01/track/2026-08-18.json?auth=<idToken>"

# apagar um dia
curl -X DELETE "https://<host>/devices/rastreador01/track/2026-08-18.json?auth=<idToken>"
```

Cada nó:

```json
{
  "lat":  -29.163412,
  "lon":  -51.519233,
  "utc":  "2026-08-18T13:45:02Z",
  "alt":  760.4,
  "kmh":  0.0,
  "sats": 9,
  "hdop": 1.20
}
```

O `utc` vem do próprio satélite (`ddmmyy` + `hhmmss` do NMEA convertidos para
ISO 8601), não do relógio do ESP32 — que começa zerado a cada reset. Sem fix
válido o firmware não envia nada, para não sujar o histórico com pontos em
`0,0`.

---

## 7. Ciclo de autenticação

```
     tem idToken válido (>5 min)?  --sim-->  usa
                  |não
     tem refreshToken na NVS?      --sim-->  POST securetoken.googleapis.com/v1/token
                  |não                        (mantém o mesmo UID)
     POST identitytoolkit.googleapis.com/v1/accounts:signUp
     (cria o usuário anônimo uma única vez, guarda o refreshToken na NVS)
```

O refreshToken é gravado na NVS justamente para **não** criar um usuário anônimo
novo por hora — sem isso o painel de Authentication acumularia ~720 contas
órfãs por mês e o UID mudaria a cada renovação.

Um 401 na escrita descarta o idToken, e o ciclo seguinte renova sozinho.

---

## 8. Problemas comuns

| Sintoma | Causa |
|---|---|
| `Auth falhou: HTTP 400 ... OPERATION_NOT_ALLOWED` | provedor Anônimo não habilitado (seção 2) |
| `HTTP 401 ... Permission denied` | regras ainda em modo bloqueado (seção 4) |
| `HTTP 404` na escrita | `FB_DB_HOST` errado — confira se copiou sem `https://` |
| `status 0` / sem resposta | handshake TLS falhou; confirme `AT+CSSLCFG="enableSNI",0,1` |
| `Sem fix valido` | GNSS sem céu aberto; TTFF a frio leva 30-90 s |
