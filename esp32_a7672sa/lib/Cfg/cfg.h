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

  // Carrega da NVS — ou volta aos padrões de fábrica se o firmware mudou.
  //
  // A NVS sobrevive à gravação do firmware, então uma unidade que teve o
  // display ligado em bancada continuava ligando depois de regravada: o
  // "padrão desligado" só valia em placa virgem. Gravar novo firmware passa a
  // significar configuração de fábrica, que é o que se espera ao montar uma
  // unidade nova.
  void carregar();
  void salvar() const;
  String paraJson() const;

  // Volta tudo ao padrão de fábrica e grava.
  void restaurarPadrao();

  // Aplica os campos presentes no JSON. Devolve true se algo mudou.
  bool aplicarJson(const String& json);
};

extern CfgUnidade cfg;
