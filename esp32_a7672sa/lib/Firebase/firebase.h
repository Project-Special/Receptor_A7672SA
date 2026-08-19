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
// Caminhos no banco:
//     /devices/<id>/last                      última posição (sobrescreve)
//     /devices/<id>/track/<AAAA-MM-DD>/<auto> histórico, um nó por dia
//     /devices/<id>/config                    LIDO: ordens vindas do app
//     /devices/<id>/status                    escrito: o que o device está fazendo
//     /devices/<id>/logs/<auto>               escrito: eventos, para o app ler
//
// O rastreamento fica DESLIGADO até o app pôr config/enabled = true. Sem isso
// um device ligado na bancada gastaria dados e escritas sem ninguém pedir.
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

  // Desligado, grava só a última posição — metade das escritas, sem histórico.
  // Equivale ao seletor "Histórico" da aba Firebase do app.
  void setTrackEnabled(bool on) { _track = on; }

  // Lê /devices/<id>/config e atualiza o estado local. Devolve false quando a
  // leitura falha — e aí o estado anterior é mantido, para uma falha de rede
  // não desligar (nem ligar) o rastreamento por conta própria.
  bool fetchConfig();

  // O device deve estar enviando posições? Vem do config lido do banco.
  bool enabled() const { return _enabled; }

  // Parâmetros vindos de /config. 0 significa "usar o padrão do firmware".
  uint32_t remoteInterval() const { return _remoteInterval; }
  uint32_t statusEvery() const    { return _statusEvery; }
  bool     trackEnabled() const   { return _track; }
  float    minDistance() const    { return _minDist; }

  // Registra um evento em /devices/<id>/logs para o app mostrar.
  // `level`: "info", "warn" ou "erro".
  bool remoteLog(const String& level, const String& message);

  // Lê /devices/<id>/wifi e guarda na NVS. Devolve true quando as credenciais
  // mudaram — aí vale reconectar. O cache local existe para o Wi-Fi não
  // depender de já haver internet: no boot as credenciais saem da NVS.
  bool fetchWifi(String& ssid, String& pass);
  static bool wifiSalvo(String& ssid, String& pass);

  // UID do usuário anônimo. Estável entre reinícios (o refreshToken fica na
  // NVS) e é o que permite restringir a leitura da senha do Wi-Fi só a este
  // device nas regras do banco.
  const String& uid() const { return _uid; }

  // Atualiza /devices/<id>/status — é o que diz ao app que o device está vivo.
  bool sendStatus(const String& utc, bool hasFix, int sats, int rssi,
                  uint32_t sent, uint32_t failed);

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
  bool fetch(const String& path, String& out);
  static String jsonRaw(const String& src, const String& key);
  bool signUpAnonymous();
  bool refreshIdToken();
  void storeTokens(const String& idToken, const String& refreshToken, long expiresIn);
  static String jsonString(const String& src, const String& key);

  A7672Tls& _t;
  String _apiKey, _dbHost, _deviceId;
  String _idToken, _refreshToken, _uid;
  String _lastError;
  uint32_t _tokenExpiresAt = 0;   // millis() em que o token vence
  bool _track = true;
  bool _enabled = false;          // só envia depois que o app autorizar
  uint32_t _remoteInterval = 0;
  uint32_t _statusEvery = 0;
  float _minDist = 0;             // metros; 0 = envia todo ciclo
  double _lastLat = 0, _lastLon = 0;
  bool _temUltimo = false;
};
