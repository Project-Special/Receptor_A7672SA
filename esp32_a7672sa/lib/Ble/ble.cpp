#include "ble.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

BleCfg ble;

static const char* UUID_SERV = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* UUID_RX   = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* UUID_TX   = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

static BLEServer*         gServer = nullptr;
static BLECharacteristic* gTx     = nullptr;
static bool               gConectado = false;
static BleLinhaHandler    gCb;
static String             gBuf;

class ServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { gConectado = true; }
  void onDisconnect(BLEServer* s) override {
    gConectado = false;
    // Sem voltar a anunciar, o aparelho some para sempre depois da primeira
    // desconexão e só um reboot o traz de volta.
    s->startAdvertising();
  }
};

class RxCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    std::string v = c->getValue();
    for (char ch : v) {
      if (ch == '\n' || ch == '\r') {
        String linha = gBuf;
        gBuf = "";
        linha.trim();
        // O callback roda na task do BLE. Os comandos daqui só mexem em NVS e
        // em variáveis, então é seguro; qualquer coisa demorada teria de ser
        // enfileirada para o loop.
        if (linha.length() && gCb) gCb(linha);
      } else if (gBuf.length() < 512) {
        gBuf += ch;
      }
    }
  }
};

bool BleCfg::begin(const String& nome, BleLinhaHandler aoReceber) {
  if (_ativo) return true;
  gCb = aoReceber;

  BLEDevice::init(nome.c_str());
  gServer = BLEDevice::createServer();
  gServer->setCallbacks(new ServerCb());

  BLEService* serv = gServer->createService(UUID_SERV);

  gTx = serv->createCharacteristic(UUID_TX, BLECharacteristic::PROPERTY_NOTIFY);
  gTx->addDescriptor(new BLE2902());

  BLECharacteristic* rx = serv->createCharacteristic(
      UUID_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCb());

  serv->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(UUID_SERV);
  adv->setScanResponse(true);
  // Sem estes mínimos, alguns iPhones não completam a conexão.
  adv->setMinPreferred(0x06);
  adv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();

  _ativo = true;
  return true;
}

void BleCfg::end() {
  if (!_ativo) return;
  BLEDevice::deinit(true);
  gServer = nullptr; gTx = nullptr; gConectado = false;
  _ativo = false;
}

bool BleCfg::conectado() const { return gConectado; }

void BleCfg::enviar(const String& linha) {
  if (!_ativo || !gConectado || !gTx) return;

  // A MTU padrão do BLE deixa ~20 bytes por notificação; o @CFG inteiro passa
  // disso, então vai em pedaços.
  String s = linha + "\n";
  const size_t passo = 20;
  for (size_t i = 0; i < s.length(); i += passo) {
    String parte = s.substring(i, min(i + passo, (size_t)s.length()));
    gTx->setValue((uint8_t*)parte.c_str(), parte.length());
    gTx->notify();
    delay(8);   // dar tempo à pilha; sem isto notificações se perdem
  }
}
