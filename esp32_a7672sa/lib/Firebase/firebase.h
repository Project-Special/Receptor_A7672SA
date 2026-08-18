// Envio de posição GNSS para o Firebase Realtime Database.
//
// Autenticação: usuário anônimo do Firebase Auth. O módulo pede um idToken via
// REST e o manda no header Authorization. O token vale 1 hora e é renovado
// sozinho — por isso o envio precisa do cliente de lib/Tls e não do AT+HTTP*,
// cujo header não passa de 287 caracteres (o token tem ~900).
//
// O refreshToken fica guardado na NVS: sem ele, cada renovação chamaria
// accounts:signUp de novo e criaria um usuário anônimo novo por hora, enchendo
// o painel de Authentication de contas órfãs e trocando o UID a cada vez.
//
// Caminhos escritos no banco:
//     /devices/<id>/last                     última posição (sobrescreve)
//     /devices/<id>/track/<AAAA-MM-DD>/<auto> histórico, um nó por dia
#pragma once

#include <tls.h>
#include <gnss.h>
#include <Preferences.h>

class A7672Firebase {
public:
  explicit A7672Firebase(A7672Tls& tls) : _t(tls) {}

  // `dbHost` é o host do banco, sem esquema:
  // "meu-projeto-default-rtdb.firebaseio.com".
  void begin(const String& apiKey, const String& dbHost, const String& deviceId);

  // Garante um idToken válido, renovando quando falta menos de 5 min. Tenta
  // primeiro o refreshToken guardado; só cai no signUp se não houver.
  bool ensureAuth();

  // Esquece o usuário anônimo salvo — o próximo ensureAuth() cria outro.
  void forgetIdentity();

  // Escreve a posição nos dois caminhos. Sem fix válido não envia nada.
  bool sendFix(const GnssFix& fix);

  bool authenticated() const { return _idToken.length() > 0; }
  const String& lastError() const { return _lastError; }

  // ddmmyy + hhmmss do NMEA -> "2026-08-18T13:45:02Z". Vazio se a data
  // ainda não veio do satélite.
  static String isoTimestamp(const String& utcDate, const String& utcTime);

  // Nó do dia dentro do histórico: "2026-08-18", ou "sem-data" enquanto o
  // receptor ainda não entregou a data.
  static String dayKey(const String& iso);

private:
  bool put(const String& path, const String& json, const String& method);
  bool signUpAnonymous();
  bool refreshIdToken();
  void storeTokens(const String& idToken, const String& refreshToken, long expiresIn);
  static String jsonString(const String& src, const String& key);

  A7672Tls& _t;
  String _apiKey, _dbHost, _deviceId;
  String _idToken, _refreshToken;
  String _lastError;
  uint32_t _tokenExpiresAt = 0;   // millis() em que o token vence
};
