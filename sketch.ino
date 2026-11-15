/*
 * Nó de Borda IoT - Climatização com Auditoria de Eficiência Térmica
 *
 * Organização em camadas (de dentro para fora):
 *   1. Domínio     -> regras do termostato e do diagnóstico (não conhece hardware)
 *   2. Adaptadores -> sensor DHT22, saídas digitais, display LCD, Wi-Fi e telemetria HTTP
 *   3. Aplicação   -> orquestra o ciclo: ler -> decidir -> atuar -> exibir -> reportar
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>

// =====================================================================
// 0. CONFIGURAÇÃO
// =====================================================================
namespace Configuracao {
  const uint8_t PINO_SENSOR_DHT = 4;
  const uint8_t PINO_RELE_COMPRESSOR = 14;  // LED azul representa o relé do compressor
  const uint8_t PINO_INDICADOR_FALHA = 13;  // LED vermelho de alarme
  const uint8_t ENDERECO_I2C_DISPLAY = 0x27;

  const char* const NOME_REDE_WIFI = "Wokwi-GUEST";
  const char* const SENHA_REDE_WIFI = "";
  // Se o Wi-Fi cair ou não conectar no boot, tenta de novo sem travar o termostato
  const unsigned long INTERVALO_ENTRE_TENTATIVAS_DE_RECONEXAO_WIFI_MS = 15000;

  const char* const NOME_SERVIDOR_TELEMETRIA = "script.google.com";
  const char* const URL_GOOGLE_APPS_SCRIPT =
      "https://script.google.com/macros/s/AKfycbyIgbD3TfE7yOYCtrhXi-DGhCPKF7QrC1RJGXHnCi2yVY3nt2QqgOkokt2zGH05w2_3/exec";

  // Testa DNS, TCP e TLS uma vez ao ligar. Desative quando o envio estiver funcionando.
  const bool EXECUTAR_DIAGNOSTICO_DE_CONEXAO_AO_INICIAR = true;

  // Histerese: liga em >= 24,0 °C e desliga em <= 22,5 °C
  const float TEMPERATURA_ALVO_CELSIUS = 23.0;
  const float MARGEM_PARA_LIGAR_CELSIUS = 1.0;
  const float MARGEM_PARA_DESLIGAR_CELSIUS = 0.5;

  // Watchdog de eficiência térmica (20 s para demonstração)
  const unsigned long TEMPO_LIMITE_PARA_RESFRIAR_MS = 20000;
  const float QUEDA_MINIMA_ESPERADA_CELSIUS = 0.5;

  const float POTENCIA_COMPRESSOR_LIGADO_WATTS = 1200.0;
  const float POTENCIA_MODO_ESPERA_WATTS = 5.0;

  const unsigned long INTERVALO_LEITURA_SENSOR_MS = 2000;   // mínimo do DHT22
  const unsigned long INTERVALO_ENVIO_TELEMETRIA_MS = 7000; // estabilidade de sockets no Wokwi

  // O handshake TLS é emulado no Wokwi e fica bem mais lento que num ESP32 real
  const int32_t TEMPO_LIMITE_DE_CONEXAO_MS = 30000;
  const uint16_t TEMPO_LIMITE_DE_RESPOSTA_MS = 15000;  // Cobre a "partida a frio" do Apps Script
  const unsigned long TEMPO_LIMITE_DO_HANDSHAKE_TLS_SEGUNDOS = 30;
}

// =====================================================================
// 1. DOMÍNIO
// =====================================================================
enum class Diagnostico : uint8_t {
  STANDBY_ECO,
  RESFRIANDO,
  RESFRIANDO_OK,
  FALHA_EFICIENCIA,
  ERRO_SENSOR_OFFLINE
};

struct LeituraAmbiente {
  bool leituraValida;
  float temperaturaCelsius;
  float umidadeRelativaPercentual;
};

struct EstadoClimatizacao {
  LeituraAmbiente leituraAmbiente;
  bool compressorLigado;
  float potenciaEstimadaWatts;
  Diagnostico diagnostico;
};

class DescricaoDiagnostico {
public:
  static const char* paraTelemetria(Diagnostico diagnostico) {
    switch (diagnostico) {
      case Diagnostico::STANDBY_ECO:         return "STANDBY_ECO";
      case Diagnostico::RESFRIANDO:          return "RESFRIANDO";
      case Diagnostico::RESFRIANDO_OK:       return "RESFRIANDO_OK";
      case Diagnostico::FALHA_EFICIENCIA:    return "FALHA_EFICIENCIA";
      case Diagnostico::ERRO_SENSOR_OFFLINE: return "ERRO_SENSOR_OFFLINE";
    }
    return "DESCONHECIDO";
  }

  static const char* paraDisplay(Diagnostico diagnostico) {
    switch (diagnostico) {
      case Diagnostico::STANDBY_ECO:         return "ECO";
      case Diagnostico::RESFRIANDO:          return "COOL";
      case Diagnostico::RESFRIANDO_OK:       return "COOL";
      case Diagnostico::FALHA_EFICIENCIA:    return "FALHA";
      case Diagnostico::ERRO_SENSOR_OFFLINE: return "ERRO";
    }
    return "?";
  }

  static bool indicaFalha(Diagnostico diagnostico) {
    return diagnostico == Diagnostico::FALHA_EFICIENCIA ||
           diagnostico == Diagnostico::ERRO_SENSOR_OFFLINE;
  }
};

class ControladorTermostato {
public:
  EstadoClimatizacao processarLeitura(const LeituraAmbiente& leituraAmbiente, unsigned long instanteAtualMs) {
    if (!leituraAmbiente.leituraValida) {
      compressorLigado = false;  // Fail-safe: sem sensor, o compressor não pode operar
      return montarEstado(leituraAmbiente, Diagnostico::ERRO_SENSOR_OFFLINE);
    }

    atualizarCompressorComHisterese(leituraAmbiente.temperaturaCelsius, instanteAtualMs);
    const Diagnostico diagnostico = calcularDiagnostico(leituraAmbiente.temperaturaCelsius, instanteAtualMs);
    return montarEstado(leituraAmbiente, diagnostico);
  }

private:
  bool compressorLigado = false;
  unsigned long instanteEmQueCompressorLigouMs = 0;
  float temperaturaQuandoCompressorLigouCelsius = 0.0;

  void atualizarCompressorComHisterese(float temperaturaCelsius, unsigned long instanteAtualMs) {
    const float temperaturaParaLigar = Configuracao::TEMPERATURA_ALVO_CELSIUS + Configuracao::MARGEM_PARA_LIGAR_CELSIUS;
    const float temperaturaParaDesligar = Configuracao::TEMPERATURA_ALVO_CELSIUS - Configuracao::MARGEM_PARA_DESLIGAR_CELSIUS;

    if (!compressorLigado && temperaturaCelsius >= temperaturaParaLigar) {
      compressorLigado = true;
      instanteEmQueCompressorLigouMs = instanteAtualMs;
      temperaturaQuandoCompressorLigouCelsius = temperaturaCelsius;
    } else if (compressorLigado && temperaturaCelsius <= temperaturaParaDesligar) {
      compressorLigado = false;
    }
  }

  Diagnostico calcularDiagnostico(float temperaturaCelsius, unsigned long instanteAtualMs) const {
    if (!compressorLigado) {
      return Diagnostico::STANDBY_ECO;
    }

    const unsigned long tempoComCompressorLigadoMs = instanteAtualMs - instanteEmQueCompressorLigouMs;
    if (tempoComCompressorLigadoMs < Configuracao::TEMPO_LIMITE_PARA_RESFRIAR_MS) {
      return Diagnostico::RESFRIANDO;
    }

    const float quedaDeTemperaturaObtida = temperaturaQuandoCompressorLigouCelsius - temperaturaCelsius;
    return quedaDeTemperaturaObtida >= Configuracao::QUEDA_MINIMA_ESPERADA_CELSIUS
               ? Diagnostico::RESFRIANDO_OK
               : Diagnostico::FALHA_EFICIENCIA;
  }

  EstadoClimatizacao montarEstado(const LeituraAmbiente& leituraAmbiente, Diagnostico diagnostico) const {
    const float potenciaEstimadaWatts = compressorLigado
                                            ? Configuracao::POTENCIA_COMPRESSOR_LIGADO_WATTS
                                            : Configuracao::POTENCIA_MODO_ESPERA_WATTS;
    return EstadoClimatizacao{leituraAmbiente, compressorLigado, potenciaEstimadaWatts, diagnostico};
  }
};

// =====================================================================
// 2. ADAPTADORES DE HARDWARE E REDE
// =====================================================================
class SensorAmbienteDHT {
public:
  explicit SensorAmbienteDHT(uint8_t pinoDeDados) : sensorDht(pinoDeDados, DHT22) {}

  void iniciar() { sensorDht.begin(); }

  LeituraAmbiente ler() {
    const float umidadeRelativaPercentual = sensorDht.readHumidity();
    const float temperaturaCelsius = sensorDht.readTemperature();
    const bool leituraValida = !isnan(umidadeRelativaPercentual) && !isnan(temperaturaCelsius);
    return LeituraAmbiente{leituraValida, temperaturaCelsius, umidadeRelativaPercentual};
  }

private:
  DHT sensorDht;
};

class SaidaDigital {
public:
  explicit SaidaDigital(uint8_t pinoDeSaida) : numeroDoPino(pinoDeSaida) {}

  void iniciar() {
    pinMode(numeroDoPino, OUTPUT);
    definirEstado(false);
  }

  void definirEstado(bool ligada) { digitalWrite(numeroDoPino, ligada ? HIGH : LOW); }

private:
  const uint8_t numeroDoPino;
};

class DisplayDeStatus {
public:
  explicit DisplayDeStatus(uint8_t enderecoI2C) : displayLcd(enderecoI2C, QUANTIDADE_DE_COLUNAS, 2) {}

  void iniciar() {
    displayLcd.init();
    displayLcd.backlight();
  }

  void mostrarMensagem(const char* textoLinhaSuperior, const char* textoLinhaInferior) {
    escreverLinha(0, textoLinhaSuperior);
    escreverLinha(1, textoLinhaInferior);
  }

  void mostrarEstado(const EstadoClimatizacao& estado) {
    if (estado.diagnostico == Diagnostico::ERRO_SENSOR_OFFLINE) {
      mostrarMensagem("ERR: SENSOR DHT", "DIAG: OFF-LINE");
      return;
    }

    char textoLinhaSuperior[QUANTIDADE_DE_COLUNAS + 1];
    char textoLinhaInferior[QUANTIDADE_DE_COLUNAS + 1];
    snprintf(textoLinhaSuperior, sizeof(textoLinhaSuperior), "T:%.1fC Sp:%.0fC",
             estado.leituraAmbiente.temperaturaCelsius, Configuracao::TEMPERATURA_ALVO_CELSIUS);
    snprintf(textoLinhaInferior, sizeof(textoLinhaInferior), "AR:%-3s D:%s",
             estado.compressorLigado ? "ON" : "OFF", DescricaoDiagnostico::paraDisplay(estado.diagnostico));
    mostrarMensagem(textoLinhaSuperior, textoLinhaInferior);
  }

private:
  static const uint8_t QUANTIDADE_DE_COLUNAS = 16;
  LiquidCrystal_I2C displayLcd;

  // Preenche a linha inteira com espaços para não sobrar lixo de mensagens anteriores
  void escreverLinha(uint8_t numeroDaLinha, const char* texto) {
    char textoComPreenchimento[QUANTIDADE_DE_COLUNAS + 1];
    snprintf(textoComPreenchimento, sizeof(textoComPreenchimento), "%-16s", texto);
    displayLcd.setCursor(0, numeroDaLinha);
    displayLcd.print(textoComPreenchimento);
  }
};

class ConexaoWifi {
public:
  ConexaoWifi(const char* nomeDaRede, const char* senhaDaRede)
      : nomeDaRede(nomeDaRede), senhaDaRede(senhaDaRede) {}

  // Conexão inicial: espera um tempo limitado e segue em frente mesmo sem rede
  void conectar() {
    Serial.printf("[WIFI] Conectando a %s", nomeDaRede);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(nomeDaRede, senhaDaRede);

    for (int tentativa = 0; tentativa < QUANTIDADE_MAXIMA_DE_VERIFICACOES && !estaConectado(); tentativa++) {
      delay(INTERVALO_ENTRE_VERIFICACOES_MS);
      Serial.print(".");
    }

    if (estaConectado()) {
      registrarConexaoEstabelecida();
    } else {
      Serial.println("\n[WIFI] Falha ao conectar. Nova tentativa automatica em segundo plano.");
    }
    instanteUltimaTentativaDeConexaoMs = millis();
  }

  // Chamado a cada ciclo: sem delay, para o controle do termostato nunca parar
  void manterConectado(unsigned long instanteAtualMs) {
    const bool conectadoAgora = estaConectado();
    if (conectadoAgora && !estavaConectadoNoUltimoCiclo) {
      registrarConexaoEstabelecida();
    }
    estavaConectadoNoUltimoCiclo = conectadoAgora;

    const bool tempoDeTentarNovamente = instanteAtualMs - instanteUltimaTentativaDeConexaoMs >=
                                        Configuracao::INTERVALO_ENTRE_TENTATIVAS_DE_RECONEXAO_WIFI_MS;
    if (conectadoAgora || !tempoDeTentarNovamente) {
      return;
    }

    Serial.println("[WIFI] Sem conexao. Tentando reconectar...");
    WiFi.disconnect();
    WiFi.begin(nomeDaRede, senhaDaRede);
    instanteUltimaTentativaDeConexaoMs = instanteAtualMs;
  }

  bool estaConectado() const { return WiFi.status() == WL_CONNECTED; }

private:
  static const int QUANTIDADE_MAXIMA_DE_VERIFICACOES = 60;          // 60 x 300 ms = 18 s
  static const unsigned long INTERVALO_ENTRE_VERIFICACOES_MS = 300;
  const char* nomeDaRede;
  const char* senhaDaRede;
  unsigned long instanteUltimaTentativaDeConexaoMs = 0;
  bool estavaConectadoNoUltimoCiclo = false;

  void registrarConexaoEstabelecida() {
    Serial.printf("\n[WIFI] Conectado. IP: %s | RSSI=%d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    estavaConectadoNoUltimoCiclo = true;
  }
};

// Testa cada camada da conexão separadamente para descobrir onde o envio falha
class DiagnosticoDeConexao {
public:
  static void executar(const char* nomeDoServidor) {
    Serial.printf("[DIAGNOSTICO] Testando conexao com %s\n", nomeDoServidor);
    if (!verificarResolucaoDeNome(nomeDoServidor)) return;
    if (!verificarConexaoTcp(nomeDoServidor)) return;
    verificarHandshakeTls(nomeDoServidor);
  }

private:
  static const uint16_t PORTA_HTTPS = 443;
  static const int32_t TEMPO_LIMITE_TCP_MS = 15000;

  static bool verificarResolucaoDeNome(const char* nomeDoServidor) {
    IPAddress enderecoIpDoServidor;
    const bool nomeResolvido = WiFi.hostByName(nomeDoServidor, enderecoIpDoServidor);
    Serial.printf("[DIAGNOSTICO] DNS: %s (%s)\n", nomeResolvido ? "OK" : "FALHOU",
                  enderecoIpDoServidor.toString().c_str());
    return nomeResolvido;
  }

  static bool verificarConexaoTcp(const char* nomeDoServidor) {
    WiFiClient clienteTcp;
    const bool conexaoAberta = clienteTcp.connect(nomeDoServidor, PORTA_HTTPS, TEMPO_LIMITE_TCP_MS);
    Serial.printf("[DIAGNOSTICO] TCP porta 443: %s\n", conexaoAberta ? "OK" : "FALHOU");
    clienteTcp.stop();
    return conexaoAberta;
  }

  static void verificarHandshakeTls(const char* nomeDoServidor) {
    WiFiClientSecure clienteSeguro;
    clienteSeguro.setInsecure();
    clienteSeguro.setHandshakeTimeout(Configuracao::TEMPO_LIMITE_DO_HANDSHAKE_TLS_SEGUNDOS);

    const unsigned long instanteInicioMs = millis();
    const bool handshakeConcluido = clienteSeguro.connect(nomeDoServidor, PORTA_HTTPS);
    const unsigned long duracaoDoHandshakeMs = millis() - instanteInicioMs;

    if (handshakeConcluido) {
      Serial.printf("[DIAGNOSTICO] TLS: OK em %lu ms\n", duracaoDoHandshakeMs);
    } else {
      char mensagemDeErroTls[120];
      clienteSeguro.lastError(mensagemDeErroTls, sizeof(mensagemDeErroTls));
      Serial.printf("[DIAGNOSTICO] TLS: FALHOU apos %lu ms -> %s\n", duracaoDoHandshakeMs, mensagemDeErroTls);
    }
    clienteSeguro.stop();
  }
};

class ClienteTelemetriaGoogleSheets {
public:
  explicit ClienteTelemetriaGoogleSheets(const char* urlDeDestino) : urlDeDestino(urlDeDestino) {}

  void enviar(const EstadoClimatizacao& estado) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[TELEMETRIA] Wi-Fi desconectado. Envio ignorado neste ciclo.");
      return;
    }

    const String corpoJson = montarCorpoJson(estado);
    for (int numeroDaTentativa = 1; numeroDaTentativa <= QUANTIDADE_MAXIMA_DE_TENTATIVAS_DE_ENVIO; numeroDaTentativa++) {
      const int codigoRespostaHttp = tentarEnviar(corpoJson);
      // Só repete quando os dados não chegaram ao servidor. Em timeout de leitura o
      // Apps Script já recebeu o JSON, e repetir gravaria uma linha duplicada.
      const bool dadosNaoChegaramAoServidor = codigoRespostaHttp == HTTPC_ERROR_CONNECTION_REFUSED;
      if (!dadosNaoChegaramAoServidor) {
        return;
      }
      Serial.printf("[TELEMETRIA] Tentativa %d de %d falhou.\n", numeroDaTentativa, QUANTIDADE_MAXIMA_DE_TENTATIVAS_DE_ENVIO);
      registrarSituacaoDaMemoria();
      delay(INTERVALO_ENTRE_TENTATIVAS_MS);
    }
  }

private:
  // Com o tempo limite maior, 2 tentativas evitam travar o termostato por tempo demais
  static const int QUANTIDADE_MAXIMA_DE_TENTATIVAS_DE_ENVIO = 2;
  static const unsigned long INTERVALO_ENTRE_TENTATIVAS_MS = 1500;
  const char* urlDeDestino;

  // O handshake TLS precisa de um bloco contínuo grande de memória (~40 KB).
  // Se o "maior bloco livre" cair muito, a falha é de memória, e não de rede.
  void registrarSituacaoDaMemoria() const {
    Serial.printf("[MEMORIA] Livre=%u bytes | Maior bloco livre=%u bytes | Wi-Fi RSSI=%d dBm\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap(), WiFi.RSSI());
  }

  // Retorna o código HTTP (>0) ou o código de erro de conexão (<0)
  int tentarEnviar(const String& corpoJson) {
    WiFiClientSecure clienteSeguro;
    clienteSeguro.setInsecure();  // Sem validação de certificado (aceitável no simulador acadêmico)
    clienteSeguro.setHandshakeTimeout(Configuracao::TEMPO_LIMITE_DO_HANDSHAKE_TLS_SEGUNDOS);

    HTTPClient clienteHttp;
    clienteHttp.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    clienteHttp.setConnectTimeout(Configuracao::TEMPO_LIMITE_DE_CONEXAO_MS);
    clienteHttp.setTimeout(Configuracao::TEMPO_LIMITE_DE_RESPOSTA_MS);

    if (!clienteHttp.begin(clienteSeguro, urlDeDestino)) {
      Serial.println("[TELEMETRIA] Nao foi possivel iniciar a conexao HTTP.");
      return HTTPC_ERROR_CONNECTION_REFUSED;
    }

    clienteHttp.addHeader("Content-Type", "application/json");
    const int codigoRespostaHttp = clienteHttp.POST(corpoJson);
    registrarResultadoDoEnvio(codigoRespostaHttp, clienteHttp, clienteSeguro);

    clienteHttp.end();
    clienteSeguro.stop();  // Libera o socket TLS para o próximo envio
    return codigoRespostaHttp;
  }

  String montarCorpoJson(const EstadoClimatizacao& estado) const {
    // Com o sensor offline enviamos null, para não poluir a auditoria com 0 °C falsos
    const bool leituraValida = estado.leituraAmbiente.leituraValida;
    const String temperaturaJson = leituraValida ? String(estado.leituraAmbiente.temperaturaCelsius, 1) : String("null");
    const String umidadeJson = leituraValida ? String(estado.leituraAmbiente.umidadeRelativaPercentual, 1) : String("null");

    String corpoJson = "{";
    corpoJson += "\"temperatura\":" + temperaturaJson + ",";
    corpoJson += "\"umidade\":" + umidadeJson + ",";
    corpoJson += "\"setpoint\":" + String(Configuracao::TEMPERATURA_ALVO_CELSIUS, 1) + ",";
    corpoJson += "\"status_compressor\":\"" + String(estado.compressorLigado ? "LIGADO" : "DESLIGADO") + "\",";
    corpoJson += "\"potencia_w\":" + String(estado.potenciaEstimadaWatts, 1) + ",";
    corpoJson += "\"diagnostico\":\"" + String(DescricaoDiagnostico::paraTelemetria(estado.diagnostico)) + "\"";
    corpoJson += "}";
    return corpoJson;
  }

  void registrarResultadoDoEnvio(int codigoRespostaHttp, HTTPClient& clienteHttp, WiFiClientSecure& clienteSeguro) const {
    // O Apps Script executa o doPost e depois responde 302 (redirecionamento para o resultado)
    const bool envioRegistrado = codigoRespostaHttp == 200 || codigoRespostaHttp == 302;
    if (envioRegistrado) {
      Serial.printf("[TELEMETRIA] Registrado na planilha (HTTP %d)\n", codigoRespostaHttp);
    } else if (codigoRespostaHttp > 0) {
      Serial.printf("[TELEMETRIA] Servidor respondeu HTTP %d\n", codigoRespostaHttp);
    } else {
      // O erro do TLS diz o motivo real por trás do "connection refused"
      char mensagemDeErroTls[120];
      clienteSeguro.lastError(mensagemDeErroTls, sizeof(mensagemDeErroTls));
      Serial.printf("[TELEMETRIA] Falha de envio: %s (codigo %d) | TLS: %s\n",
                    clienteHttp.errorToString(codigoRespostaHttp).c_str(), codigoRespostaHttp, mensagemDeErroTls);
    }
  }
};

// =====================================================================
// 3. APLICAÇÃO
// =====================================================================
class AplicacaoClimatizacao {
public:
  void iniciar() {
    Serial.begin(115200);

    releCompressor.iniciar();
    indicadorDeFalha.iniciar();
    displayDeStatus.iniciar();
    displayDeStatus.mostrarMensagem("Iniciando IoT...", "Conectando WiFi");
    sensorAmbiente.iniciar();

    conexaoWifi.conectar();
    executarDiagnosticoDeConexaoUmaVez();
    displayDeStatus.mostrarMensagem("Sistema pronto", conexaoWifi.estaConectado() ? "WiFi: OK" : "WiFi: OFFLINE");
  }

  void executarCiclo() {
    const unsigned long instanteAtualMs = millis();

    conexaoWifi.manterConectado(instanteAtualMs);
    executarDiagnosticoDeConexaoUmaVez();

    if (intervaloFoiAtingido(instanteUltimaLeituraMs, Configuracao::INTERVALO_LEITURA_SENSOR_MS, instanteAtualMs)) {
      processarNovaLeitura(instanteAtualMs);
    }

    if (possuiEstadoProcessado &&
        intervaloFoiAtingido(instanteUltimoEnvioMs, Configuracao::INTERVALO_ENVIO_TELEMETRIA_MS, instanteAtualMs)) {
      clienteTelemetria.enviar(estadoAtual);
    }
  }

private:
  SensorAmbienteDHT sensorAmbiente{Configuracao::PINO_SENSOR_DHT};
  SaidaDigital releCompressor{Configuracao::PINO_RELE_COMPRESSOR};
  SaidaDigital indicadorDeFalha{Configuracao::PINO_INDICADOR_FALHA};
  DisplayDeStatus displayDeStatus{Configuracao::ENDERECO_I2C_DISPLAY};
  ConexaoWifi conexaoWifi{Configuracao::NOME_REDE_WIFI, Configuracao::SENHA_REDE_WIFI};
  ClienteTelemetriaGoogleSheets clienteTelemetria{Configuracao::URL_GOOGLE_APPS_SCRIPT};
  ControladorTermostato controladorTermostato;

  EstadoClimatizacao estadoAtual{};
  bool possuiEstadoProcessado = false;
  unsigned long instanteUltimaLeituraMs = 0;
  unsigned long instanteUltimoEnvioMs = 0;
  bool diagnosticoDeConexaoJaExecutado = false;

  // Roda na primeira vez que o Wi-Fi conecta, mesmo que isso aconteça depois do boot
  void executarDiagnosticoDeConexaoUmaVez() {
    const bool deveExecutarDiagnostico = Configuracao::EXECUTAR_DIAGNOSTICO_DE_CONEXAO_AO_INICIAR &&
                                         !diagnosticoDeConexaoJaExecutado && conexaoWifi.estaConectado();
    if (!deveExecutarDiagnostico) {
      return;
    }
    displayDeStatus.mostrarMensagem("Testando rede...", "Aguarde");
    DiagnosticoDeConexao::executar(Configuracao::NOME_SERVIDOR_TELEMETRIA);
    diagnosticoDeConexaoJaExecutado = true;
  }

  bool intervaloFoiAtingido(unsigned long& instanteUltimaExecucaoMs, unsigned long intervaloMs, unsigned long instanteAtualMs) {
    if (instanteAtualMs - instanteUltimaExecucaoMs < intervaloMs) {
      return false;
    }
    instanteUltimaExecucaoMs = instanteAtualMs;
    return true;
  }

  void processarNovaLeitura(unsigned long instanteAtualMs) {
    const LeituraAmbiente leituraAmbiente = sensorAmbiente.ler();
    const EstadoClimatizacao novoEstado = controladorTermostato.processarLeitura(leituraAmbiente, instanteAtualMs);

    // O LED de falha é derivado do diagnóstico atual, então ele apaga sozinho quando o problema some
    releCompressor.definirEstado(novoEstado.compressorLigado);
    indicadorDeFalha.definirEstado(DescricaoDiagnostico::indicaFalha(novoEstado.diagnostico));
    displayDeStatus.mostrarEstado(novoEstado);
    registrarNoMonitorSerial(novoEstado);

    estadoAtual = novoEstado;
    possuiEstadoProcessado = true;
  }

  void registrarNoMonitorSerial(const EstadoClimatizacao& novoEstado) const {
    const bool diagnosticoMudou = !possuiEstadoProcessado || novoEstado.diagnostico != estadoAtual.diagnostico;
    if (diagnosticoMudou) {
      Serial.printf("[EVENTO] Diagnostico alterado para %s\n", DescricaoDiagnostico::paraTelemetria(novoEstado.diagnostico));
    }

    if (!novoEstado.leituraAmbiente.leituraValida) {
      Serial.println("[FALHA] Sensor DHT22 sem resposta. Compressor desligado (fail-safe).");
      return;
    }

    Serial.printf("[LEITURA] T=%.1fC U=%.1f%% compressor=%s potencia=%.0fW diagnostico=%s\n",
                  novoEstado.leituraAmbiente.temperaturaCelsius,
                  novoEstado.leituraAmbiente.umidadeRelativaPercentual,
                  novoEstado.compressorLigado ? "LIGADO" : "DESLIGADO",
                  novoEstado.potenciaEstimadaWatts,
                  DescricaoDiagnostico::paraTelemetria(novoEstado.diagnostico));
  }
};

AplicacaoClimatizacao aplicacaoClimatizacao;

void setup() {
  aplicacaoClimatizacao.iniciar();
}

void loop() {
  aplicacaoClimatizacao.executarCiclo();
}
