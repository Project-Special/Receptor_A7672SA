// Cliente HTTP(S) sobre AT+HTTP*.
#pragma once

#include <core.h>
#include <net.h>

struct HttpResponse {
  bool   ok = false;
  int    status = 0;       // código HTTP, ou 6xx nos erros do próprio módulo
  size_t length = 0;       // bytes anunciados pelo +HTTPACTION
  String body;
};

class A7672Http {
public:
  A7672Http(A7672Core& core, A7672Net& net) : _c(core), _n(net) {}

  HttpResponse get(const String& url);
  HttpResponse post(const String& url, const String& body,
                    const String& contentType = "application/json");

  // Corpos grandes são lidos em blocos: pedir tudo de uma vez estoura o buffer
  // da UART antes do ESP32 drenar. 0 = sem limite de tamanho armazenado.
  void setChunkSize(size_t bytes) { _chunk = bytes; }
  void setMaxBody(size_t bytes)   { _maxBody = bytes; }
  void setTimeout(uint32_t ms)    { _actionTimeout = ms; }

private:
  HttpResponse request(int method, const String& url,
                       const String& body, const String& contentType);
  bool readBody(size_t total, String& out);

  A7672Core& _c;
  A7672Net&  _n;
  size_t   _chunk = 1024;
  size_t   _maxBody = 8192;
  uint32_t _actionTimeout = 60000;
};
