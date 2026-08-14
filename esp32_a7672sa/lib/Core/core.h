// Núcleo de comunicação com o SIMCom A7672SA.
// Transporte UART, motor de comandos AT, despacho de URCs e estado do SIM.
#pragma once

#include <Arduino.h>
#include <functional>
#include <vector>

enum class SimState : uint8_t { Unknown, Ready, Absent, Locked };

// URCs chegam a qualquer momento, inclusive no meio da resposta de outro
// comando (NMEA, +CMTI, RING). Quem se interessa se registra aqui.
using UrcHandler = std::function<void(const String&)>;

class A7672Core {
public:
  bool begin(HardwareSerial& port, int rxPin, int txPin, uint32_t baud = 115200);

  // PWRKEY do módulo é ativo em BAIXO com pulso de ~50 ms (Ton do datasheet).
  // Placas com transistor invertem isso: a Muz 24x24, por exemplo, é ativa em
  // ALTO. Depois do pulso a UART leva ~8 s para responder.
  void powerOn(int pwrkeyPin, bool activeHigh, uint32_t pulseMs = 60);

  // Manda AT até o módulo responder. Use após powerOn().
  bool waitReady(uint32_t timeoutMs = 15000);

  // Envia o comando e consome até OK/ERROR. true só em OK.
  bool at(const String& cmd, uint32_t timeoutMs = 8000);

  // Como at(), mas devolve a primeira linha que começar com `prefix`.
  // String vazia se não vier.
  String queryLine(const String& cmd, const String& prefix, uint32_t timeoutMs = 8000);

  // Espera uma linha que comece com `token`, despachando o resto como URC.
  bool waitToken(const String& token, uint32_t timeoutMs);

  // Prompt '>' do CMGS/HTTPDATA chega sem quebra de linha.
  bool waitPrompt(uint32_t timeoutMs = 6000);

  void write(const String& s);
  void writeRaw(const uint8_t* data, size_t len);

  // Drena a UART e despacha URCs. Chame no loop() quando não houver
  // comando em andamento, senão as URCs só aparecem no próximo comando.
  void pump(uint32_t forMs = 0);

  // Lê uma linha crua. Exposto porque o HTTP precisa contar bytes do corpo.
  bool readLine(String& out, uint32_t timeoutMs);

  void addUrcHandler(UrcHandler h) { _urc.push_back(h); }

  SimState sim() const { return _sim; }
  bool refreshSim();                        // consulta AT+CPIN? e atualiza
  bool requireSim(const char* what);        // bloqueia se ausente/travado
  static const char* simText(SimState s);

  void setDebug(Stream* s) { _dbg = s; }
  const String& lastError() const { return _lastError; }

private:
  void dispatch(const String& line);
  void trackSim(const String& line);
  static bool isFinal(const String& l);

  HardwareSerial* _ser = nullptr;
  Stream* _dbg = nullptr;
  String _buf;
  String _lastError;
  SimState _sim = SimState::Unknown;
  std::vector<UrcHandler> _urc;
};
