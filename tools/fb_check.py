"""
Verifica a configuração do Firebase sem depender do hardware.

Faz do PC exatamente o que o A7672SA fará: cria um usuário anônimo, escreve a
última posição, grava um ponto no nó do dia e lê tudo de volta. Cada etapa que
falha aponta qual passo do firebase.md ficou pendente.

Uso:
    python tools/fb_check.py --api-key AIza... --db-host meu-rastreador-default-rtdb.firebaseio.com
    python tools/fb_check.py ... --device rastreador01 --keep
"""
import argparse
import json
import sys
import urllib.error
import urllib.request
from datetime import datetime, timezone

AUTH = "https://identitytoolkit.googleapis.com/v1/accounts:signUp?key={key}"
REFRESH = "https://securetoken.googleapis.com/v1/token?key={key}"

OK, FAIL, INFO = "  [ok]  ", "  [FALHA]", "  ..."


def call(url, method="GET", payload=None, form=False):
    data = None
    headers = {}
    if payload is not None:
        if form:
            data = payload.encode()
            headers["Content-Type"] = "application/x-www-form-urlencoded"
        else:
            data = json.dumps(payload).encode()
            headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            body = r.read().decode()
            return r.status, (json.loads(body) if body.strip() else None)
    except urllib.error.HTTPError as e:
        body = e.read().decode()
        try:
            return e.code, json.loads(body)
        except json.JSONDecodeError:
            return e.code, {"raw": body[:300]}
    except urllib.error.URLError as e:
        return 0, {"raw": str(e)}


def fail(msg, hint=""):
    print(FAIL, msg)
    if hint:
        print("         ->", hint)
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--api-key", required=True, help="Web API Key (Configuracoes do projeto -> Geral)")
    ap.add_argument("--db-host", required=True, help="host do RTDB, sem https:// e sem barra final")
    ap.add_argument("--device", default="teste-fb-check")
    ap.add_argument("--keep", action="store_true", help="nao apagar os dados de teste no fim")
    a = ap.parse_args()

    host = a.db_host.replace("https://", "").replace("http://", "").rstrip("/")
    if host != a.db_host:
        print(INFO, f"host normalizado para {host}")

    print(f"\n=== Firebase check — {host} ===\n")

    # 1. Autenticacao anonima -----------------------------------------------
    print("1. Auth anonimo")
    status, body = call(AUTH.format(key=a.api_key), "POST", {"returnSecureToken": True})
    if status != 200:
        msg = (body or {}).get("error", {}).get("message", body)
        if msg == "OPERATION_NOT_ALLOWED":
            fail("provedor Anonimo desabilitado",
                 "Console -> Authentication -> Sign-in method -> habilitar Anonimo")
        if "API key not valid" in str(msg):
            fail("Web API Key invalida",
                 "Console -> Configuracoes do projeto -> Geral -> Chave de API da Web")
        fail(f"HTTP {status}: {msg}")

    id_token = body["idToken"]
    uid = body.get("localId", "?")
    print(OK, f"uid {uid}, idToken com {len(id_token)} chars")
    if len(id_token) > 600:
        print(INFO, "confirma por que o firmware usa lib/Tls: nao cabe no AT+HTTPPARA (max 600)")

    # 2. Refresh do token ----------------------------------------------------
    print("\n2. Refresh do token (mesmo caminho que o firmware usa depois de 1 h)")
    status, body2 = call(REFRESH.format(key=a.api_key), "POST",
                         f"grant_type=refresh_token&refresh_token={body['refreshToken']}",
                         form=True)
    if status == 200 and body2.get("id_token"):
        print(OK, "refresh funcionou — o UID se mantem entre renovacoes")
    else:
        print(FAIL, f"refresh HTTP {status}: {body2}")

    # 3. Escrita da ultima posicao ------------------------------------------
    now = datetime.now(timezone.utc)
    day = now.strftime("%Y-%m-%d")
    ponto = {
        "lat": -29.163412, "lon": -51.519233,
        "utc": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "alt": 760.4, "kmh": 0.0, "sats": 9, "hdop": 1.20,
    }
    base = f"https://{host}/devices/{a.device}"
    auth_q = f"?auth={id_token}"

    print(f"\n3. PUT {base.split(host)[1]}/last.json")
    status, body3 = call(f"{base}/last.json{auth_q}", "PUT", ponto)
    if status != 200:
        msg = (body3 or {}).get("error", body3)
        if status == 401:
            fail(f"401 {msg}", "regras ainda bloqueadas — rode: firebase deploy --only database")
        if status == 404:
            fail("404 — host do banco errado",
                 "confira a URL no console do Realtime Database (sem https://)")
        fail(f"HTTP {status}: {msg}")
    print(OK, "ultima posicao gravada")

    # 4. Escrita no no do dia -----------------------------------------------
    print(f"4. POST /devices/{a.device}/track/{day}.json")
    status, body4 = call(f"{base}/track/{day}.json{auth_q}", "POST", ponto)
    if status != 200:
        fail(f"HTTP {status}: {(body4 or {}).get('error', body4)}")
    print(OK, f"ponto no no do dia (chave {body4.get('name')})")

    # 5. Leitura de volta ----------------------------------------------------
    print("\n5. Leitura")
    status, last = call(f"{base}/last.json{auth_q}")
    print(OK if status == 200 else FAIL, f"last -> {json.dumps(last)[:100]}")
    status, track = call(f"{base}/track/{day}.json{auth_q}")
    n = len(track) if isinstance(track, dict) else 0
    print(OK if status == 200 else FAIL, f"track/{day} -> {n} ponto(s)")

    # 6. Rejeicao de dado invalido ------------------------------------------
    print("\n6. Regras rejeitam lixo?")
    status, _ = call(f"{base}/track/{day}.json{auth_q}", "POST", {"lat": 999, "lon": 0})
    if status == 200:
        print(FAIL, "lat=999 foi aceita — as regras de validacao nao subiram")
    else:
        print(OK, f"lat=999 recusada (HTTP {status})")

    # 7. Limpeza -------------------------------------------------------------
    if a.keep:
        print(f"\n   dados de teste mantidos em /devices/{a.device}")
    else:
        call(f"{base}.json{auth_q}", "DELETE")
        print(f"\n   dados de teste removidos de /devices/{a.device}")

    print("\n=== tudo pronto — pode gravar o firmware ===")
    print(f"    FB_API_KEY = \"{a.api_key[:10]}...\"")
    print(f"    FB_DB_HOST = \"{host}\"")


if __name__ == "__main__":
    main()
