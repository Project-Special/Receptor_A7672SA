// Cliente HTTPS sobre o canal SSL cru (AT+CCH*).
//
// Por que não usar o AT+HTTP* de lib/Http?  Porque o cliente HTTP embutido no
// A7672SA impõe limites medidos neste módulo (fw A7672M7_B19V01_250905):
//
//     AT+HTTPPARA="URL",...       aceita no máximo  600 caracteres
//     AT+HTTPPARA="USERDATA",...  aceita no máximo  287 caracteres
//
// Um idToken do Firebase Auth tem ~900 caracteres, então não cabe nem na query
// string nem no header Authorization. O canal SSL cru não tem esse teto: o
// request-line e os headers são bytes que nós mesmos montamos.
#pragma once

#include <core.h>
#include <net.h>

struct TlsResponse {
  bool   ok = false;
  int    status = 0;      // código HTTP; 0 = não chegou resposta
  String body;
};

class A7672Tls {
public:
  A7672Tls(A7672Core& core, A7672Net& net) : _c(core), _n(net) {}

  // Sobe o serviço de canais SSL e configura o contexto. Idempotente.
  bool begin();
  void end();

  // Faz uma requisição completa: abre o socket, envia, lê e fecha.
  // `extraHeaders` entra cru, cada header terminado em \r\n.
  TlsResponse request(const String& method, const String& host, const String& path,
                      const String& body = "", const String& contentType = "",
                      const String& extraHeaders = "");

  void setTimeout(uint32_t ms)  { _timeout = ms; }
  void setMaxBody(size_t bytes) { _maxBody = bytes; }

private:
  bool open(const String& host, uint16_t port);
  void close();
  bool sendAll(const String& data);
  bool collect(String& raw);
  static bool parse(const String& raw, TlsResponse& out, size_t maxBody);
  static String dechunk(const String& in);

  bool dataReadyCached();

  A7672Core& _c;
  A7672Net&  _n;
  bool     _started = false;
  uint32_t _timeout = 30000;
  size_t   _maxBody = 4096;
  uint32_t _dataOkAt = 0;      // millis() da última confirmação de PDP ativo
};
