// Configuração pelo celular, sem cabo e sem internet.
//
// Usa o BLE do próprio ESP32-S3, não o Bluetooth do A7672. Os dois rádios não
// se encaixam no mesmo caso de uso: o módulo tem Bluetooth clássico com SPP,
// que o iOS não expõe a apps comuns e que navegador nenhum alcança — a Web
// Bluetooth só fala BLE/GATT. Já o BLE do ESP32-S3 é acessível pelo próprio
// app web no celular.
//
// O serviço é o NUS (Nordic UART Service), que é o "porta serial sobre BLE"
// que praticamente todo app de terminal BLE já conhece:
//
//     serviço  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
//     RX       6E400002-…  (write)  celular -> device
//     TX       6E400003-…  (notify) device -> celular
//
// Aceita os mesmos comandos do cabo: @CFG, @WIFI, @SCAN, @LOG, @PING.
#pragma once

#include <Arduino.h>
#include <functional>

using BleLinhaHandler = std::function<void(const String&)>;

class BleCfg {
public:
  // `nome` é como o aparelho aparece na varredura do celular.
  bool begin(const String& nome, BleLinhaHandler aoReceber);
  void end();

  bool ativo() const    { return _ativo; }
  bool conectado() const;

  // Envia uma linha ao celular. Quebra em pedaços que caibam na MTU.
  void enviar(const String& linha);

private:
  bool _ativo = false;
  BleLinhaHandler _cb;
};

extern BleCfg ble;
