"""
A7672SA Serial Bridge — WebSocket <-> porta serial (pyserial)

Permite usar o A7672SA Controller em navegadores sem Web Serial
(Firefox, Safari, Android) e em máquinas remotas na mesma rede.

Protocolo (JSON por mensagem):
  navegador -> bridge : {"type":"list"}
                        {"type":"open","port":"COM7","baud":115200}
                        {"type":"tx","data":"AT\r\n"}
                        {"type":"close"}
  bridge -> navegador : {"type":"ports","ports":[{"device":..,"description":..}]}
                        {"type":"status","state":"open|closed|error",...}
                        {"type":"rx","data":"...."}

Uso:
  pip install websockets pyserial
  python a7672_bridge.py                # escuta em 0.0.0.0:8765
  python a7672_bridge.py --port COM7 --baud 115200 --host 127.0.0.1
"""

import argparse
import asyncio
import json
import subprocess
import sys

# ── Dependências ──────────────────────────────────────────────────────────────
def pip_install(*pkgs):
    subprocess.check_call([sys.executable, "-m", "pip", "install", *pkgs],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

try:
    import websockets
except ImportError:
    print("Instalando websockets...")
    pip_install("websockets")
    import websockets  # type: ignore

try:
    import serial
    import serial.tools.list_ports as list_ports
except ImportError:
    print("Instalando pyserial...")
    pip_install("pyserial")
    import serial
    import serial.tools.list_ports as list_ports


DEFAULT_BAUD = 115200
READ_CHUNK = 4096


class SerialLink:
    """Porta serial não bloqueante lida por uma task asyncio."""

    def __init__(self):
        self.ser: "serial.Serial | None" = None
        self.reader_task: "asyncio.Task | None" = None

    @property
    def is_open(self) -> bool:
        return self.ser is not None and self.ser.is_open

    def open(self, port: str, baud: int):
        self.close()
        self.ser = serial.Serial(
            port=port, baudrate=baud, bytesize=8, parity="N", stopbits=1,
            timeout=0, write_timeout=2, rtscts=False, dsrdtr=False,
        )
        # Alguns módulos A76XX ficam mudos se DTR/RTS estiverem em nível errado
        try:
            self.ser.dtr = True
            self.ser.rts = True
        except Exception:
            pass

    def close(self):
        if self.reader_task and not self.reader_task.done():
            self.reader_task.cancel()
        self.reader_task = None
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None

    def write(self, text: str):
        if not self.is_open:
            raise RuntimeError("porta fechada")
        self.ser.write(text.encode("utf-8", errors="replace"))
        self.ser.flush()


def scan_ports():
    out = []
    for p in list_ports.comports():
        out.append({
            "device": p.device,
            "description": p.description or "",
            "hwid": p.hwid or "",
            "manufacturer": p.manufacturer or "",
        })
    # Portas típicas do A7672SA aparecem como "AT Interface" / "USB Serial Device"
    out.sort(key=lambda x: ("AT" not in x["description"], x["device"]))
    return out


async def pump_serial(link: SerialLink, ws):
    """Lê a serial continuamente e repassa ao navegador."""
    loop = asyncio.get_running_loop()
    while link.is_open:
        try:
            n = link.ser.in_waiting
            data = await loop.run_in_executor(None, link.ser.read, n or 1) if n else b""
            if not n:
                await asyncio.sleep(0.02)
                continue
            if data:
                text = data.decode("utf-8", errors="replace")
                await ws.send(json.dumps({"type": "rx", "data": text}))
        except (serial.SerialException, OSError) as e:
            await ws.send(json.dumps({"type": "status", "state": "error", "error": str(e)}))
            link.close()
            break
        except asyncio.CancelledError:
            break
        except Exception as e:  # noqa: BLE001
            print(f"[bridge] erro de leitura: {e}")
            await asyncio.sleep(0.1)


async def handle_client(ws, default_port=None, default_baud=DEFAULT_BAUD):
    peer = getattr(ws, "remote_address", ("?", 0))
    print(f"[bridge] cliente conectado: {peer}")
    link = SerialLink()

    async def open_port(port, baud):
        try:
            link.open(port, baud)
        except Exception as e:  # noqa: BLE001
            await ws.send(json.dumps({"type": "status", "state": "error", "error": str(e)}))
            print(f"[bridge] falha ao abrir {port}: {e}")
            return
        await ws.send(json.dumps({"type": "status", "state": "open", "port": port, "baud": baud}))
        print(f"[bridge] {port} aberta @ {baud}")
        link.reader_task = asyncio.create_task(pump_serial(link, ws))

    try:
        async for raw in ws:
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                continue

            kind = msg.get("type")

            if kind == "list":
                await ws.send(json.dumps({"type": "ports", "ports": scan_ports()}))

            elif kind == "open":
                port = msg.get("port") or default_port
                baud = int(msg.get("baud") or default_baud)
                if not port:
                    ports = scan_ports()
                    port = ports[0]["device"] if ports else None
                if not port:
                    await ws.send(json.dumps({"type": "status", "state": "error",
                                              "error": "nenhuma porta serial encontrada"}))
                    continue
                await open_port(port, baud)

            elif kind == "tx":
                data = msg.get("data", "")
                try:
                    link.write(data)
                except Exception as e:  # noqa: BLE001
                    await ws.send(json.dumps({"type": "status", "state": "error", "error": str(e)}))

            elif kind == "close":
                link.close()
                await ws.send(json.dumps({"type": "status", "state": "closed"}))

    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        link.close()
        print(f"[bridge] cliente desconectado: {peer}")


async def main():
    ap = argparse.ArgumentParser(description="Bridge WebSocket <-> serial para o A7672SA")
    ap.add_argument("--host", default="0.0.0.0", help="endereço de escuta (padrão 0.0.0.0)")
    ap.add_argument("--ws-port", type=int, default=8765, help="porta do WebSocket (padrão 8765)")
    ap.add_argument("--port", default=None, help="porta serial padrão, ex: COM7 ou /dev/ttyUSB2")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="baud padrão (115200)")
    args = ap.parse_args()

    print("Portas seriais detectadas:")
    for p in scan_ports():
        print(f"  {p['device']:<12} {p['description']}")
    print(f"\n[bridge] WebSocket em ws://{args.host}:{args.ws_port}")
    print("[bridge] No navegador escolha 'WebSocket Bridge (Python)' e conecte.\n")

    handler = lambda ws: handle_client(ws, args.port, args.baud)  # noqa: E731
    async with websockets.serve(handler, args.host, args.ws_port, max_size=2 ** 22):
        await asyncio.Future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[bridge] encerrado.")
