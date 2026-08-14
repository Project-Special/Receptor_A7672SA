// Rede LTE/GSM: registro, sinal, APN e contexto PDP.
#pragma once

#include <core.h>

struct NetStatus {
  int   rssi   = 99;      // 0–31, 99 = desconhecido
  int   dbm    = 0;       // derivado do rssi
  int   rsrp   = 0;
  int   rsrq   = 0;
  int   cereg  = 0;       // 1 = registrado, 5 = roaming
  bool  attached = false;
  String tech;            // "LTE", "GSM", "NO SERVICE"
  String operatorName;
  String ip;
};

class A7672Net {
public:
  explicit A7672Net(A7672Core& core) : _c(core) {}

  // Diagnóstico puro — funciona sem SIM e é o que se quer olhar quando o SIM
  // é justamente o problema. Por isso não passa pelo requireSim().
  bool refresh(NetStatus& out);

  int  signalDbm();                       // -113..-51, ou 0 se desconhecido
  bool registered();

  // Estes exigem SIM.
  bool setApn(const String& apn, const String& user = "", const String& pass = "");
  bool attach(uint32_t timeoutMs = 30000);
  bool waitRegistered(uint32_t timeoutMs = 60000);

  // Contexto PDP ativo com IP válido: pré-requisito de HTTP/MQTT/TCP.
  bool dataReady(String* ipOut = nullptr);

  static int rssiToDbm(int rssi);

private:
  A7672Core& _c;
};
