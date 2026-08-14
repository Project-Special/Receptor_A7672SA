// SMS em modo texto, GSM e UCS2.
#pragma once

#include <core.h>

struct SmsMessage {
  int    index = 0;
  String status;      // "REC UNREAD", "REC READ", ...
  String sender;
  String timestamp;
  String text;
};

// Disparado pela URC +CMTI: chegou mensagem no índice informado.
using SmsArrivedHandler = std::function<void(int index, const String& storage)>;

class A7672Sms {
public:
  explicit A7672Sms(A7672Core& core) : _c(core) {}

  // Modo texto, memória SIM e URC de chegada (+CMTI).
  bool begin();

  // Acentos exigem UCS2. Na prática: com ucs2=true tanto o corpo quanto o
  // número vão em hex UTF-16BE, que é o que os SIMCom esperam sob CSCS="UCS2".
  bool send(const String& number, const String& text, bool ucs2 = false);

  bool read(int index, SmsMessage& out);
  bool deleteAll();
  bool deleteOne(int index);

  void onArrived(SmsArrivedHandler h);

  // Conta septetos GSM: '€', '{', '}', '[', ']', '~', '\', '|', '^' ocupam 2.
  static size_t gsmSeptets(const String& text);
  static size_t partsFor(const String& text, bool ucs2);

  static String toUcs2Hex(const String& s);

private:
  A7672Core& _c;
  SmsArrivedHandler _cb = nullptr;
  bool _urcHooked = false;
};
