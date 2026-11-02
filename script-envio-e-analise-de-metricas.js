/**
 * Backend de Telemetria IoT - Google Apps Script (vinculado à planilha)
 *
 * Responsabilidades:
 *   1. Receber as leituras do ESP32 (POST com JSON) e gravar na aba "Telemetria".
 *   2. Avisar por e-mail quando uma nova falha começa.
 *   3. Manter a aba "Relatorio_Falhas" sempre atualizada (status atual, custo e eventos).
 */

const CONFIGURACAO = {
  NOME_ABA_TELEMETRIA: "Telemetria",
  NOME_ABA_RELATORIO_FALHAS: "Relatorio_Falhas",
  DIAGNOSTICO_FALHA_EFICIENCIA: "FALHA_EFICIENCIA",
  DIAGNOSTICO_SENSOR_OFFLINE: "ERRO_SENSOR_OFFLINE",
  TARIFA_ENERGIA_REAIS_POR_KWH: 0.90, // Troque pelo valor da sua conta de energia
  INTERVALO_NOMINAL_ENTRE_LEITURAS_SEGUNDOS: 7,
  INTERVALO_MAXIMO_ENTRE_LEITURAS_SEGUNDOS: 60, // Acima disso, considera que a simulação foi parada
  HORAS_DE_EXPEDIENTE_POR_DIA: 8,
  DIAS_UTEIS_POR_MES: 22,
  ALERTAS_POR_EMAIL_ATIVADOS: true,
  EMAIL_DESTINATARIO_DOS_ALERTAS: "", // Vazio = e-mail do dono da planilha
  TEMPO_MAXIMO_DE_ESPERA_DA_TRAVA_MS: 10000
};

const INDICE_COLUNA_TELEMETRIA = {
  DATA_HORA: 0,
  TEMPERATURA: 1,
  UMIDADE: 2,
  SETPOINT: 3,
  COMPRESSOR: 4,
  POTENCIA: 5,
  DIAGNOSTICO: 6
};

const DESCRICAO_DAS_FALHAS = {
  FALHA_EFICIENCIA: "Compressor ligado sem queda de temperatura. Possível vazamento de fluido refrigerante ou defeito mecânico. O equipamento consome carga máxima sem refrigerar.",
  ERRO_SENSOR_OFFLINE: "Sensor de temperatura sem resposta. O compressor foi desligado por segurança (fail-safe)."
};

const COR = {
  FUNDO_FALHA: "#F4C7C3",
  FUNDO_SENSOR_OFFLINE: "#FCE8B2",
  FUNDO_NORMAL: "#D9EAD3",
  FUNDO_SEM_DADOS: "#EFEFEF",
  CABECALHO: "#0B6E5F",
  TEXTO_CABECALHO: "#FFFFFF"
};

const SEGUNDOS_POR_HORA = 3600;
const SEGUNDOS_POR_DIA = 86400;

// =====================================================================
// 1. RECEPÇÃO DA TELEMETRIA
// =====================================================================
function doPost(evento) {
  const travaDeExecucao = LockService.getScriptLock();
  try {
    travaDeExecucao.waitLock(CONFIGURACAO.TEMPO_MAXIMO_DE_ESPERA_DA_TRAVA_MS);

    const leituraRecebida = extrairLeituraDoCorpoJson(evento);
    const diagnosticoAnterior = obterUltimoDiagnosticoRegistrado();
    registrarLeituraNaTelemetria(leituraRecebida);

    executarSemInterromperRegistro("alerta por e-mail", () => notificarSeIniciouNovaFalha(diagnosticoAnterior, leituraRecebida));
    executarSemInterromperRegistro("atualização do relatório", gerarRelatorioDeFalhas);

    return criarRespostaEmTexto("OK");
  } catch (erro) {
    console.error("Falha ao registrar telemetria: " + erro.message);
    return criarRespostaEmTexto("ERRO: " + erro.message);
  } finally {
    travaDeExecucao.releaseLock();
  }
}

// GET serve apenas como verificação de saúde: abrir a URL no navegador não grava linhas vazias
function doGet() {
  return criarRespostaEmTexto("Endpoint de telemetria ativo. Envie as leituras via POST com JSON.");
}

function extrairLeituraDoCorpoJson(evento) {
  const corpoDaRequisicao = evento && evento.postData ? evento.postData.contents : "";
  if (!corpoDaRequisicao) {
    throw new Error("Requisição sem corpo JSON.");
  }

  const dadosRecebidos = JSON.parse(corpoDaRequisicao);
  return {
    temperaturaCelsius: obterValorOuVazio(dadosRecebidos.temperatura),
    umidadeRelativaPercentual: obterValorOuVazio(dadosRecebidos.umidade),
    temperaturaAlvoCelsius: obterValorOuVazio(dadosRecebidos.setpoint),
    statusCompressor: obterValorOuVazio(dadosRecebidos.status_compressor),
    potenciaWatts: obterValorOuVazio(dadosRecebidos.potencia_w),
    diagnostico: String(obterValorOuVazio(dadosRecebidos.diagnostico)).trim()
  };
}

function obterValorOuVazio(valor) {
  return valor === undefined || valor === null ? "" : valor;
}

function obterUltimoDiagnosticoRegistrado() {
  const abaTelemetria = obterOuCriarAba(CONFIGURACAO.NOME_ABA_TELEMETRIA);
  const numeroDaUltimaLinha = abaTelemetria.getLastRow();
  if (numeroDaUltimaLinha < 2) {
    return "";
  }
  const colunaDoDiagnostico = INDICE_COLUNA_TELEMETRIA.DIAGNOSTICO + 1;
  return String(abaTelemetria.getRange(numeroDaUltimaLinha, colunaDoDiagnostico).getValue()).trim();
}

function registrarLeituraNaTelemetria(leitura) {
  const abaTelemetria = obterOuCriarAba(CONFIGURACAO.NOME_ABA_TELEMETRIA);
  abaTelemetria.appendRow([
    new Date(),
    leitura.temperaturaCelsius,
    leitura.umidadeRelativaPercentual,
    leitura.temperaturaAlvoCelsius,
    leitura.statusCompressor,
    leitura.potenciaWatts,
    leitura.diagnostico
  ]);
}

// Uma falha no e-mail ou no relatório não pode impedir que a leitura fique registrada
function executarSemInterromperRegistro(nomeDaEtapa, etapa) {
  try {
    etapa();
  } catch (erro) {
    console.error("Falha na etapa de " + nomeDaEtapa + ": " + erro.message);
  }
}

// =====================================================================
// 2. ALERTAS
// =====================================================================
function diagnosticoGeraAlerta(diagnostico) {
  return diagnostico === CONFIGURACAO.DIAGNOSTICO_FALHA_EFICIENCIA ||
         diagnostico === CONFIGURACAO.DIAGNOSTICO_SENSOR_OFFLINE;
}

// Avisa só na transição para a falha, e não a cada 7 segundos enquanto ela dura
function notificarSeIniciouNovaFalha(diagnosticoAnterior, leituraAtual) {
  const iniciouNovaFalha = diagnosticoGeraAlerta(leituraAtual.diagnostico) &&
                           leituraAtual.diagnostico !== diagnosticoAnterior;
  if (!iniciouNovaFalha || !CONFIGURACAO.ALERTAS_POR_EMAIL_ATIVADOS) {
    return;
  }
  enviarEmailDeAlerta(leituraAtual);
}

function enviarEmailDeAlerta(leitura) {
  const emailDoDestinatario = CONFIGURACAO.EMAIL_DESTINATARIO_DOS_ALERTAS || Session.getEffectiveUser().getEmail();
  const assuntoDoEmail = "[ALERTA IoT] " + leitura.diagnostico + " no ar-condicionado";
  const corpoDoEmail = [
    "Uma nova falha foi detectada pelo nó de borda IoT.",
    "",
    "Diagnóstico: " + leitura.diagnostico,
    "O que significa: " + DESCRICAO_DAS_FALHAS[leitura.diagnostico],
    "",
    "Temperatura: " + (leitura.temperaturaCelsius === "" ? "indisponível" : leitura.temperaturaCelsius + " °C"),
    "Setpoint: " + leitura.temperaturaAlvoCelsius + " °C",
    "Compressor: " + leitura.statusCompressor,
    "Potência estimada: " + leitura.potenciaWatts + " W",
    "Detectado em: " + Utilities.formatDate(new Date(), Session.getScriptTimeZone(), "dd/MM/yyyy HH:mm:ss"),
    "",
    "Relatório completo: " + SpreadsheetApp.getActiveSpreadsheet().getUrl()
  ].join("\n");

  MailApp.sendEmail(emailDoDestinatario, assuntoDoEmail, corpoDoEmail);
}

// =====================================================================
// 3. AUDITORIA DE FALHAS (RELATÓRIO)
// =====================================================================
function onOpen() {
  SpreadsheetApp.getUi()
    .createMenu("Auditoria IoT")
    .addItem("Gerar relatório de falhas", "gerarRelatorioDeFalhas")
    .addItem("Configurar projeto (executar uma vez)", "configurarProjeto")
    .addToUi();
}

function gerarRelatorioDeFalhas() {
  const leiturasDaTelemetria = lerLeiturasDaTelemetria();
  const eventosDeFalha = agruparEventosDeFalha(leiturasDaTelemetria);
  const resumoDaAuditoria = calcularResumoDaAuditoria(eventosDeFalha);
  const ultimaLeitura = leiturasDaTelemetria[leiturasDaTelemetria.length - 1] || null;
  escreverRelatorioDeFalhas(ultimaLeitura, resumoDaAuditoria, eventosDeFalha);
}

function lerLeiturasDaTelemetria() {
  const abaTelemetria = obterOuCriarAba(CONFIGURACAO.NOME_ABA_TELEMETRIA);
  const linhasSemCabecalho = abaTelemetria.getDataRange().getValues().slice(1);

  return linhasSemCabecalho
    .filter(linha => linha[INDICE_COLUNA_TELEMETRIA.DATA_HORA] instanceof Date)
    .map(linha => ({
      dataHora: linha[INDICE_COLUNA_TELEMETRIA.DATA_HORA],
      temperaturaCelsius: Number(linha[INDICE_COLUNA_TELEMETRIA.TEMPERATURA]),
      potenciaWatts: Number(linha[INDICE_COLUNA_TELEMETRIA.POTENCIA]),
      diagnostico: String(linha[INDICE_COLUNA_TELEMETRIA.DIAGNOSTICO]).trim()
    }))
    .sort((leituraA, leituraB) => leituraA.dataHora - leituraB.dataHora);
}

// Cada linha "vale" o tempo até a próxima leitura; intervalos longos indicam simulação parada
function calcularDuracaoDaLeituraEmSegundos(leituras, indiceDaLeitura) {
  const proximaLeitura = leituras[indiceDaLeitura + 1];
  if (!proximaLeitura) {
    return CONFIGURACAO.INTERVALO_NOMINAL_ENTRE_LEITURAS_SEGUNDOS;
  }

  const diferencaEmSegundos = (proximaLeitura.dataHora - leituras[indiceDaLeitura].dataHora) / 1000;
  return diferencaEmSegundos <= CONFIGURACAO.INTERVALO_MAXIMO_ENTRE_LEITURAS_SEGUNDOS
    ? diferencaEmSegundos
    : CONFIGURACAO.INTERVALO_NOMINAL_ENTRE_LEITURAS_SEGUNDOS;
}

function agruparEventosDeFalha(leituras) {
  const eventosDeFalha = [];
  let eventoEmAndamento = null;

  leituras.forEach((leitura, indiceDaLeitura) => {
    if (leitura.diagnostico !== CONFIGURACAO.DIAGNOSTICO_FALHA_EFICIENCIA) {
      eventoEmAndamento = null;
      return;
    }

    const duracaoDaLeituraEmSegundos = calcularDuracaoDaLeituraEmSegundos(leituras, indiceDaLeitura);
    const leituraContinuaEventoAnterior = eventoEmAndamento !== null &&
      (leitura.dataHora - eventoEmAndamento.dataHoraFim) / 1000 <= CONFIGURACAO.INTERVALO_MAXIMO_ENTRE_LEITURAS_SEGUNDOS;

    if (!leituraContinuaEventoAnterior) {
      eventoEmAndamento = {
        dataHoraInicio: leitura.dataHora,
        dataHoraFim: leitura.dataHora,
        duracaoEmSegundos: 0,
        energiaDesperdicadaKwh: 0,
        temperaturaMaximaCelsius: leitura.temperaturaCelsius
      };
      eventosDeFalha.push(eventoEmAndamento);
    }

    eventoEmAndamento.dataHoraFim = new Date(leitura.dataHora.getTime() + duracaoDaLeituraEmSegundos * 1000);
    eventoEmAndamento.duracaoEmSegundos += duracaoDaLeituraEmSegundos;
    eventoEmAndamento.energiaDesperdicadaKwh += (leitura.potenciaWatts / 1000) * (duracaoDaLeituraEmSegundos / SEGUNDOS_POR_HORA);
    eventoEmAndamento.temperaturaMaximaCelsius = Math.max(eventoEmAndamento.temperaturaMaximaCelsius, leitura.temperaturaCelsius);
  });

  return eventosDeFalha;
}

function calcularResumoDaAuditoria(eventosDeFalha) {
  const duracaoTotalEmSegundos = eventosDeFalha.reduce((soma, evento) => soma + evento.duracaoEmSegundos, 0);
  const energiaTotalDesperdicadaKwh = eventosDeFalha.reduce((soma, evento) => soma + evento.energiaDesperdicadaKwh, 0);

  const potenciaMediaDuranteFalhaKw = duracaoTotalEmSegundos > 0
    ? energiaTotalDesperdicadaKwh / (duracaoTotalEmSegundos / SEGUNDOS_POR_HORA)
    : 0;
  const energiaProjetadaMensalKwh = potenciaMediaDuranteFalhaKw *
    CONFIGURACAO.HORAS_DE_EXPEDIENTE_POR_DIA * CONFIGURACAO.DIAS_UTEIS_POR_MES;

  return {
    quantidadeDeEventos: eventosDeFalha.length,
    duracaoTotalEmSegundos: duracaoTotalEmSegundos,
    energiaTotalDesperdicadaKwh: energiaTotalDesperdicadaKwh,
    custoTotalDesperdicadoReais: energiaTotalDesperdicadaKwh * CONFIGURACAO.TARIFA_ENERGIA_REAIS_POR_KWH,
    energiaProjetadaMensalKwh: energiaProjetadaMensalKwh,
    custoProjetadoMensalReais: energiaProjetadaMensalKwh * CONFIGURACAO.TARIFA_ENERGIA_REAIS_POR_KWH
  };
}

function descreverStatusAtual(ultimaLeitura) {
  if (!ultimaLeitura) {
    return { texto: "SEM LEITURAS REGISTRADAS", corDeFundo: COR.FUNDO_SEM_DADOS };
  }
  if (ultimaLeitura.diagnostico === CONFIGURACAO.DIAGNOSTICO_FALHA_EFICIENCIA) {
    return { texto: "EM FALHA: compressor ligado sem resfriar", corDeFundo: COR.FUNDO_FALHA };
  }
  if (ultimaLeitura.diagnostico === CONFIGURACAO.DIAGNOSTICO_SENSOR_OFFLINE) {
    return { texto: "SENSOR OFFLINE: compressor desligado (fail-safe)", corDeFundo: COR.FUNDO_SENSOR_OFFLINE };
  }
  return { texto: "OPERANDO NORMALMENTE (" + ultimaLeitura.diagnostico + ")", corDeFundo: COR.FUNDO_NORMAL };
}

function escreverRelatorioDeFalhas(ultimaLeitura, resumo, eventosDeFalha) {
  const abaRelatorio = obterOuCriarAba(CONFIGURACAO.NOME_ABA_RELATORIO_FALHAS);
  abaRelatorio.clear();

  const statusAtual = descreverStatusAtual(ultimaLeitura);
  const linhasDoResumo = [
    ["Auditoria de Eficiência Térmica", ""],
    ["Status atual do equipamento", statusAtual.texto],
    ["Última leitura recebida", ultimaLeitura ? ultimaLeitura.dataHora : "-"],
    ["Relatório atualizado em", new Date()],
    ["Eventos de falha detectados", resumo.quantidadeDeEventos],
    ["Tempo total em falha (h:mm:ss)", resumo.duracaoTotalEmSegundos / SEGUNDOS_POR_DIA],
    ["Energia desperdiçada (kWh)", resumo.energiaTotalDesperdicadaKwh],
    ["Tarifa considerada (R$/kWh)", CONFIGURACAO.TARIFA_ENERGIA_REAIS_POR_KWH],
    ["Custo do desperdício (R$)", resumo.custoTotalDesperdicadoReais],
    ["Projeção mensal sem detecção (kWh)", resumo.energiaProjetadaMensalKwh],
    ["Projeção mensal sem detecção (R$)", resumo.custoProjetadoMensalReais]
  ];
  abaRelatorio.getRange(1, 1, linhasDoResumo.length, 2).setValues(linhasDoResumo);

  abaRelatorio.getRange("A1:B1").merge().setFontWeight("bold").setFontSize(13)
    .setBackground(COR.CABECALHO).setFontColor(COR.TEXTO_CABECALHO);
  abaRelatorio.getRange("A2:B2").setFontWeight("bold").setBackground(statusAtual.corDeFundo);
  abaRelatorio.getRange("B3:B4").setNumberFormat("dd/MM/yyyy HH:mm:ss");
  abaRelatorio.getRange("B5").setNumberFormat("0");
  abaRelatorio.getRange("B6").setNumberFormat("[h]:mm:ss");
  abaRelatorio.getRange("B7").setNumberFormat("0.0000");
  abaRelatorio.getRange("B8:B9").setNumberFormat("[$R$ ]#,##0.00");
  abaRelatorio.getRange("B10").setNumberFormat("0.0");
  abaRelatorio.getRange("B11").setNumberFormat("[$R$ ]#,##0.00");
  abaRelatorio.getRange("A9:B9").setFontWeight("bold");
  abaRelatorio.getRange("A11:B11").setFontWeight("bold");
  abaRelatorio.getRange(2, 2, linhasDoResumo.length - 1, 1).setHorizontalAlignment("right");

  const linhaDoCabecalhoDeEventos = linhasDoResumo.length + 2;
  escreverTabelaDeEventos(abaRelatorio, linhaDoCabecalhoDeEventos, eventosDeFalha);
  definirLargurasDasColunas(abaRelatorio);
}

function escreverTabelaDeEventos(abaRelatorio, linhaDoCabecalho, eventosDeFalha) {
  const cabecalhoDosEventos = ["Início", "Fim", "Duração", "Temp. máxima (°C)", "Energia (kWh)", "Custo (R$)"];
  abaRelatorio.getRange(linhaDoCabecalho, 1, 1, cabecalhoDosEventos.length)
    .setValues([cabecalhoDosEventos])
    .setFontWeight("bold")
    .setBackground(COR.CABECALHO)
    .setFontColor(COR.TEXTO_CABECALHO);

  if (eventosDeFalha.length === 0) {
    abaRelatorio.getRange(linhaDoCabecalho + 1, 1).setValue("Nenhum evento de falha registrado.");
    return;
  }

  // Eventos mais recentes primeiro, para a banca ver a última falha no topo
  const eventosDoMaisRecente = eventosDeFalha.slice().reverse();
  const linhasDosEventos = eventosDoMaisRecente.map(evento => [
    evento.dataHoraInicio,
    evento.dataHoraFim,
    evento.duracaoEmSegundos / SEGUNDOS_POR_DIA,
    evento.temperaturaMaximaCelsius,
    evento.energiaDesperdicadaKwh,
    evento.energiaDesperdicadaKwh * CONFIGURACAO.TARIFA_ENERGIA_REAIS_POR_KWH
  ]);

  const primeiraLinhaDosEventos = linhaDoCabecalho + 1;
  const quantidadeDeEventos = linhasDosEventos.length;
  abaRelatorio.getRange(primeiraLinhaDosEventos, 1, quantidadeDeEventos, cabecalhoDosEventos.length).setValues(linhasDosEventos);
  abaRelatorio.getRange(primeiraLinhaDosEventos, 1, quantidadeDeEventos, 2).setNumberFormat("dd/MM/yyyy HH:mm:ss");
  abaRelatorio.getRange(primeiraLinhaDosEventos, 3, quantidadeDeEventos, 1).setNumberFormat("[h]:mm:ss");
  abaRelatorio.getRange(primeiraLinhaDosEventos, 4, quantidadeDeEventos, 1).setNumberFormat("0.0");
  abaRelatorio.getRange(primeiraLinhaDosEventos, 5, quantidadeDeEventos, 1).setNumberFormat("0.0000");
  abaRelatorio.getRange(primeiraLinhaDosEventos, 6, quantidadeDeEventos, 1).setNumberFormat("[$R$ ]#,##0.00");
}

// Larguras fixas são mais rápidas que autoResize, o que importa porque roda a cada telemetria
function definirLargurasDasColunas(abaRelatorio) {
  const larguraDasColunasEmPixels = [260, 300, 90, 130, 110, 100];
  larguraDasColunasEmPixels.forEach((largura, indice) => abaRelatorio.setColumnWidth(indice + 1, largura));
}

// =====================================================================
// 4. CONFIGURAÇÃO INICIAL (executar uma vez pelo menu)
// =====================================================================
function configurarProjeto() {
  formatarColunasDaTelemetria();
  destacarLinhasDeFalhaNaTelemetria();
  gerarRelatorioDeFalhas();
  MailApp.getRemainingDailyQuota(); // Força o pedido de autorização para envio de e-mail
  SpreadsheetApp.getActiveSpreadsheet().toast("Projeto configurado. Lembre-se de publicar uma nova versão da implantação.", "Auditoria IoT", 8);
}

function formatarColunasDaTelemetria() {
  const abaTelemetria = obterOuCriarAba(CONFIGURACAO.NOME_ABA_TELEMETRIA);
  abaTelemetria.getRange("A2:A").setNumberFormat("dd/MM/yyyy HH:mm:ss");
  abaTelemetria.getRange("B2:D").setNumberFormat("0.0"); // Corrige o "4000%" da umidade
  abaTelemetria.getRange("F2:F").setNumberFormat("0");
}

function destacarLinhasDeFalhaNaTelemetria() {
  const abaTelemetria = obterOuCriarAba(CONFIGURACAO.NOME_ABA_TELEMETRIA);
  const intervaloDosDados = abaTelemetria.getRange("A2:G");

  const regraFalhaDeEficiencia = SpreadsheetApp.newConditionalFormatRule()
    .whenFormulaSatisfied('=$G2="' + CONFIGURACAO.DIAGNOSTICO_FALHA_EFICIENCIA + '"')
    .setBackground(COR.FUNDO_FALHA)
    .setRanges([intervaloDosDados])
    .build();

  const regraSensorOffline = SpreadsheetApp.newConditionalFormatRule()
    .whenFormulaSatisfied('=$G2="' + CONFIGURACAO.DIAGNOSTICO_SENSOR_OFFLINE + '"')
    .setBackground(COR.FUNDO_SENSOR_OFFLINE)
    .setRanges([intervaloDosDados])
    .build();

  abaTelemetria.setConditionalFormatRules([regraFalhaDeEficiencia, regraSensorOffline]);
}

// =====================================================================
// 5. UTILITÁRIOS
// =====================================================================
function obterOuCriarAba(nomeDaAba) {
  const planilha = SpreadsheetApp.getActiveSpreadsheet();
  return planilha.getSheetByName(nomeDaAba) || planilha.insertSheet(nomeDaAba);
}

function criarRespostaEmTexto(mensagem) {
  return ContentService.createTextOutput(mensagem).setMimeType(ContentService.MimeType.TEXT);
}