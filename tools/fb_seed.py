"""
Grava uma trilha de demonstração no Firebase, para conferir o modo "Firebase"
do mapa sem depender do GNSS.

Gera pontos ao longo de um percurso em Caxias do Sul (RS), no mesmo formato que
o firmware e o app escrevem.

Uso:
    python tools/fb_seed.py --api-key AIza... --db-host <host> --device demo-trilha
    python tools/fb_seed.py ... --clear        # apaga tudo do device e sai
"""
import argparse
import json
import math
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone

AUTH = "https://identitytoolkit.googleapis.com/v1/accounts:signUp?key={key}"


def call(url, method="GET", payload=None):
    data = json.dumps(payload).encode() if payload is not None else None
    headers = {"Content-Type": "application/json"} if data else {}
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            body = r.read().decode()
            return r.status, (json.loads(body) if body.strip() else None)
    except urllib.error.HTTPError as e:
        return e.code, {"raw": e.read().decode()[:200]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--api-key", required=True)
    ap.add_argument("--db-host", required=True)
    ap.add_argument("--device", default="demo-trilha")
    ap.add_argument("--pontos", type=int, default=60)
    ap.add_argument("--dias", type=int, default=2, help="quantos dias gerar")
    ap.add_argument("--clear", action="store_true")
    a = ap.parse_args()

    host = a.db_host.replace("https://", "").rstrip("/")
    status, body = call(AUTH.format(key=a.api_key), "POST", {"returnSecureToken": True})
    if status != 200:
        print("falha no auth:", body)
        return
    token = body["idToken"]
    base = f"https://{host}/devices/{a.device}"

    if a.clear:
        call(f"{base}.json?auth={token}", "DELETE")
        print(f"apagado: /devices/{a.device}")
        return

    # Percurso simples: uma volta suave partindo do centro de Caxias do Sul.
    lat0, lon0 = -29.168, -51.179
    total = 0
    for d in range(a.dias):
        dia_base = datetime.now(timezone.utc) - timedelta(days=d)
        dia = dia_base.strftime("%Y-%m-%d")
        for i in range(a.pontos):
            t = i / max(1, a.pontos - 1)
            ang = t * 2 * math.pi
            lat = lat0 + 0.012 * math.sin(ang) + 0.004 * t
            lon = lon0 + 0.018 * math.cos(ang) * t
            ts = dia_base.replace(hour=8, minute=0, second=0, microsecond=0) + timedelta(seconds=i * 60)
            ponto = {
                "lat": round(lat, 6),
                "lon": round(lon, 6),
                "utc": ts.strftime("%Y-%m-%dT%H:%M:%SZ"),
                "alt": round(760 + 40 * math.sin(ang * 2), 1),
                "kmh": round(20 + 25 * abs(math.sin(ang * 3)), 1),
                "sats": 8 + (i % 5),
                "hdop": round(0.8 + 0.6 * abs(math.cos(ang)), 2),
            }
            st, _ = call(f"{base}/track/{dia}.json?auth={token}", "POST", ponto)
            if st != 200:
                print(f"  falha no ponto {i} de {dia}: HTTP {st}")
                return
            total += 1
            if i == a.pontos - 1:
                call(f"{base}/last.json?auth={token}", "PUT", ponto)
        print(f"  {dia}: {a.pontos} pontos")

    print(f"\n{total} pontos gravados em /devices/{a.device}")
    print("No app: aba Mapa -> seletor 'Firebase' -> escolha o dia.")
    print(f"Para remover: python tools/fb_seed.py --api-key ... --db-host ... --device {a.device} --clear")


if __name__ == "__main__":
    main()
