// Configuração da unidade — mora na NVS, entra pelo cabo.
//
// Nada disto passa pelo Firebase. São escolhas de montagem e operação do
// aparelho, feitas com ele na bancada; mandá-las pela nuvem custava uma
// requisição HTTPS a cada 30 s no chip IoT, só para descobrir que nada mudou,
// e ainda punha a configuração de campo na dependência de haver rede.
//
// O banco continua recebendo os DADOS (last, track, status, logs).
//
// Protocolo na serial USB:
//     @CFG?                          devolve tudo em JSON
//     @CFG {"interval":30,...}       aplica os campos presentes e grava
#pragma once

#include <Arduino.h>

struct CfgUnidade {
  bool     enviar      = false;   // rastreamento ligado?
  bool     historico   = true;    // grava a trilha, além da última posição
  bool     display     = false;   // há tela acoplada nesta unidade
  uint32_t intervalo   = 30;      // s entre envios
  uint32_t statusCada  = 60;      // s entre heartbeats
  float    distMin     = 0;       // m; 0 = envia todo ciclo
  int      tzMin       = -180;    // fuso em minutos

  void carregar();
  void salvar() const;
  String paraJson() const;

  // Aplica os campos presentes no JSON. Devolve true se algo mudou.
  bool aplicarJson(const String& json);
};

extern CfgUnidade cfg;
