// Voz: discagem, atendimento, DTMF e eventos de chamada.
#pragma once

#include <core.h>

enum class CallState : uint8_t { Idle, Dialing, Ringing, Active, Busy, NoAnswer };

using CallEventHandler = std::function<void(CallState state, const String& number)>;

class A7672Voice {
public:
  explicit A7672Voice(A7672Core& core) : _c(core) {}

  // Habilita a identificação do chamador (+CLIP) e engata as URCs.
  bool begin();

  // O ';' final é obrigatório: sem ele o módulo tenta uma chamada de DADOS.
  bool dial(const String& number);
  bool answer();
  bool hangup();

  bool setVolume(int level);          // 0–5 via AT+CLVL
  bool setAudioDevice(int device);    // AT+CSDVC
  bool sendDtmf(const String& digits);

  CallState state() const { return _state; }
  const String& number() const { return _number; }
  uint32_t durationSec() const;       // 0 quando não há chamada ativa

  void onEvent(CallEventHandler h) { _cb = h; }

private:
  void handleUrc(const String& line);
  void setState(CallState s);

  A7672Core& _c;
  CallEventHandler _cb = nullptr;
  CallState _state = CallState::Idle;
  String _number;
  uint32_t _startedAt = 0;
  bool _urcHooked = false;
};
