/*
  ============================================================
   ESP2 - MOTOR  -  Projeto "Pulseira Monitor de Motor"
   Placa: ESP32 DevKit (o "tradicional", 30 ou 38 pinos)
   Autor: Noberto
  ============================================================

  O que este código faz:
    1. Lê temperatura e umidade do sensor DHT11 a cada 2 segundos
       (módulo "Temp and humidity" do kit / KY-015).
    2. Lê a vibração do motor com o MPU6050, 500 vezes por segundo, e
       a cada 1 segundo calcula o quanto a máquina está vibrando.
    3. Decide os alarmes: temperatura alta (LIMITE_ALARME) e vibração
       alta (LIMITE_VIBRACAO).
    4. A cada 1 segundo envia tudo pelo ESP-NOW (o rádio do próprio
       ESP32, sem roteador e sem internet) para a pulseira (ESP1).
    5. Mostra tudo na tela TFT 1.8": temperatura, umidade, máxima e
       mínima, vibração, gráfico dos últimos 15 minutos e os alarmes.

  Ligações:
    DHT11 (KY-015)  S -> D4      meio (+) -> 3V3   - -> GND
    TFT 1.8"        VCC -> 3V3   GND -> GND        CS -> D5     RESET -> D22
                    A0 -> D21    SDA -> D23        SCK -> D18   LED -> 3V3
    MPU6050         VCC -> 3V3   GND -> GND        SCL -> D32   SDA -> D33
                    (XDA, XCL, AD0 e INT não ligam)
  O módulo do DHT11 já tem resistor: não precisa de resistor extra.
  O MPU6050 tem que ficar bem preso na máquina (parafuso, fita dupla face
  forte ou abraçadeira): solto, ele balança sozinho e mede errado.

  Bibliotecas (Arduino IDE > Gerenciador de Bibliotecas):
    - DHT sensor library                 (Adafruit)
    - Adafruit Unified Sensor            (Adafruit; o IDE oferece junto)
    - Adafruit GFX Library
    - Adafruit ST7735 and ST7789 Library
  O MPU6050 é lido direto, sem biblioteca (assim funciona também com os clones).

  Placa: "ESP32 Dev Module" (pacote esp32 da Espressif, versão 3.x)

  Limite do DHT11: mede de 0 a 50 °C (±2 °C). Ele mede o AR em volta
  do motor; não cole numa superfície mais quente que 50 °C.
*/

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <DHT.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

#if !defined(ESP_ARDUINO_VERSION_MAJOR) || ESP_ARDUINO_VERSION_MAJOR < 3
#error "Atualize o pacote de placas esp32 para a versao 3.x (Ferramentas > Placa > Gerenciador de Placas)"
#endif

// ======================= CONFIGURAÇÕES =======================

// Alarmes: quem decide é este ESP (motor). A pulseira só obedece.

// Temperatura. O DHT11 mede só até 50 °C: mantenha o limite abaixo disso.
const float LIMITE_ALARME = 35.0;     // °C: a partir daqui a pulseira bipa
const float HISTERESE     = 1.0;      // o alarme só desliga 1 °C abaixo do limite

// Vibração, em g (1 g = a força da gravidade). Para acertar o limite: com a
// máquina funcionando normal, veja o "VIB" na tela e use 2 a 3 vezes esse valor.
const float   LIMITE_VIBRACAO        = 0.50;   // g: acima disso a pulseira bipa
const uint8_t SEGUNDOS_PARA_LIGAR    = 2;      // precisa passar do limite 2 s seguidos
const uint8_t SEGUNDOS_PARA_DESLIGAR = 3;      // e ficar 3 s abaixo de 80% do limite para parar

#define PINO_SENSOR         4      // fio "S" do módulo DHT11 (D4 na placa)
#define TIPO_SENSOR         DHT11
#define ID_MOTOR            1      // número deste motor (1 a 255)
#define CANAL_ESPNOW        1      // canal de rádio: tem que ser IGUAL na pulseira (ESP1)
#define INTERVALO_DHT_MS    2000   // o DHT11 só mede a cada 2 s: não diminua
#define FALHAS_ATE_ERRO     3      // leituras falhas seguidas até avisar a pulseira

// MPU6050 (vibração): I2C em pinos próprios, porque o D21 e o D22 são da tela
#define MPU_SDA             33
#define MPU_SCL             32
#define AMOSTRA_US          2000   // lê o acelerômetro a cada 2 ms (500 vezes por segundo)
#define JANELA_MS           1000   // a cada 1 s calcula a vibração e envia para a pulseira

// Tela TFT 1.8" (ST7735) - SPI do ESP32: SDA = GPIO 23, SCK = GPIO 18 (fixos)
#define TFT_CS        5
#define TFT_RST       22
#define TFT_DC        21               // pino "A0" da tela
#define TIPO_TELA     INITR_BLACKTAB   // cores trocadas ou borda com lixo? use INITR_GREENTAB ou INITR_REDTAB
#define ROTACAO_TELA  1                // 1 ou 3 (deitada). Ficou de cabeça para baixo? troque

// LED azul da placa: pisca a cada envio (GPIO 2 no DevKit de 30 pinos; -1 = sem LED)
const int PINO_LED = 2;

// ============ PACOTE (tem que ser IGUAL nos dois códigos) ============
#define PACOTE_MAGICO 0x4E425431UL   // "NBT1": marca que o pacote é nosso
#define PACOTE_VERSAO 4              // versão 4 = com a vibração do MPU6050

typedef struct __attribute__((packed)) {
  uint32_t magico;          // sempre PACOTE_MAGICO
  uint8_t  versao;          // PACOTE_VERSAO
  uint8_t  idMotor;         // ID_MOTOR
  uint8_t  sensorOk;        // 1 = DHT11 com leitura válida | 0 = sensor com problema
  uint8_t  umidade;         // umidade relativa em % (255 = sem leitura)
  uint32_t sequencia;       // 1, 2, 3... a pulseira usa para contar perdas
  float    temperaturaC;    // temperatura em °C
  uint32_t ligadoHaSeg;     // há quantos segundos este ESP32 está ligado
  float    limiteC;         // LIMITE_ALARME
  float    vibracaoG;       // vibração do último segundo, em g
  float    limiteVibracaoG; // LIMITE_VIBRACAO
  uint8_t  alarme;          // 1 = temperatura muito alta: a pulseira bipa
  uint8_t  vibracaoOk;      // 1 = MPU6050 respondendo
  uint8_t  alarmeVibracao;  // 1 = vibração muito alta: a pulseira bipa
} PacoteMotor;

#define LARGURA_TELA       160
#define ALTURA_TELA        128
#define TAM_HISTORICO      128     // pontos do gráfico (1 por pixel)
#define INTERVALO_HIST_MS  7000    // 1 ponto a cada 7 s -> 128 pontos = ~15 min

// ======================= OBJETOS E VARIÁVEIS =======================
DHT sensor(PINO_SENSOR, TIPO_SENSOR);
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 *tela = nullptr;   // desenha aqui e manda pronto (sem piscar)

const uint8_t ENDERECO_BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint32_t sequencia = 0;
bool     estadoLed = false;
bool     ultimoEnvioOk = true;
const char *ultimaSituacao = "";

// Temperatura (DHT11)
uint8_t  falhasSeguidas = 0;
bool     leituraFalhou = false;   // a última leitura falhou (avisa no Monitor Serial)
float    ultimaTemperatura = NAN;
float    ultimaUmidade = NAN;
bool     dhtPronto = false;       // já leu o DHT11 (ou desistiu depois de 3 falhas)
bool     dhtOk = false;           // tem leitura válida de temperatura
bool     alarmeTemp = false;
bool     temMinMax = false;
float    tempMin = 0, tempMax = 0;
uint32_t ultimaLeituraDht = 0;

// Vibração (MPU6050)
uint8_t  enderecoMpu = 0;         // 0 = não encontrado
bool     mpuOk = false;           // o MPU6050 respondeu no último segundo
float    vibracaoAtual = NAN;     // em g
bool     alarmeVib = false;
uint8_t  segundosAcima = 0, segundosAbaixo = 0;
int32_t  somaX = 0, somaY = 0, somaZ = 0;      // somas das medidas do segundo atual
int64_t  somaQX = 0, somaQY = 0, somaQZ = 0;   // somas dos quadrados
uint32_t nAmostras = 0;
uint32_t proximaAmostraUs = 0;
uint32_t inicioJanela = 0;
uint8_t  janelasSemMpu = 0;

// Histórico para o gráfico
float    historico[TAM_HISTORICO];
int      nHistorico = 0;
uint32_t ultimoPontoHist = 0;

// Tela
bool     precisaRedesenhar = true;
uint32_t ultimoSegundoDesenhado = 0xFFFFFFFF;
bool     ultimoPisca = false;

// ======================= TELA =======================
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define COR_FUNDO       RGB565(8, 10, 14)
#define COR_BARRA       RGB565(26, 32, 42)
#define COR_LINHA       RGB565(48, 56, 68)
#define COR_TEXTO       RGB565(236, 240, 244)
#define COR_TEXTO_FRACO RGB565(132, 142, 154)
#define COR_VERDE       RGB565(64, 214, 120)
#define COR_AMARELO     RGB565(255, 190, 40)
#define COR_VERMELHO    RGB565(255, 64, 52)
#define COR_CINZA       RGB565(112, 118, 128)

// Situação mostrada na "pílula": alarmes primeiro, depois o erro do sensor
const char *textoSituacao() {
  if (!dhtPronto)              return "INICIANDO";
  if (alarmeTemp && alarmeVib) return "TEMP+VIB";
  if (alarmeTemp)              return "TEMP ALTA";
  if (alarmeVib)               return "VIBRACAO";
  if (!dhtOk)                  return "ERRO SENSOR";
  return "NORMAL";
}

uint16_t corSituacao() {
  if (!dhtPronto)              return COR_CINZA;
  if (alarmeTemp || alarmeVib) return COR_VERMELHO;
  if (!dhtOk)                  return COR_AMARELO;
  return COR_VERDE;
}

// Largura de um texto na fonte atual (e o deslocamento da primeira letra)
uint16_t larguraTexto(GFXcanvas16 &c, const char *txt, int16_t *deslocX) {
  int16_t x1, y1;
  uint16_t w, h;
  c.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  if (deslocX) {
    *deslocX = x1;
  }
  return w;
}

// 46 -> "46" | 45.5 -> "45.5"
void formatarGraus(char *saida, size_t tam, float v) {
  if (fabsf(v - roundf(v)) < 0.05f) {
    snprintf(saida, tam, "%.0f", v);
  } else {
    snprintf(saida, tam, "%.1f", v);
  }
}

// Tempo ligado: 0:12:34 | 12h34m | 3d07h
void formatarLigado(char *saida, size_t tam, uint32_t seg) {
  unsigned long h = seg / 3600, m = (seg / 60) % 60, s = seg % 60;
  if (h < 10) {
    snprintf(saida, tam, "%lu:%02lu:%02lu", h, m, s);
  } else if (h < 100) {
    snprintf(saida, tam, "%luh%02lum", h, m);
  } else {
    snprintf(saida, tam, "%lud%02luh", h / 24, h % 24);
  }
}

// Gráfico dos últimos ~15 minutos. A escala (máx/mín) fica numa coluna à esquerda.
void desenharGrafico(GFXcanvas16 &c, int16_t x0, int16_t gy, int16_t larguraTotal, int16_t gh, uint16_t corLinha) {
  const int16_t colunaEscala = 20;
  int16_t gx = x0 + colunaEscala;
  int16_t gw = larguraTotal - colunaEscala;
  // procura mínimo e máximo dos pontos válidos
  float vMin = 1000, vMax = -1000;
  int validos = 0;
  for (int i = 0; i < nHistorico; i++) {
    float v = historico[i];
    if (isnan(v)) continue;
    if (v < vMin) vMin = v;
    if (v > vMax) vMax = v;
    validos++;
  }
  c.drawFastHLine(gx, gy + gh, gw, COR_LINHA);   // linha de base
  c.drawFastVLine(gx - 2, gy, gh + 1, COR_LINHA);
  c.setFont(nullptr);
  c.setTextSize(1);
  c.setTextColor(COR_TEXTO_FRACO);
  if (validos < 2) {
    c.setCursor(gx + 4, gy + gh / 2 - 4);
    c.print("grafico: 15 min");
    return;
  }
  // arredonda a escala para graus inteiros, com faixa mínima de 4 °C
  vMin = floorf(vMin);
  vMax = ceilf(vMax);
  if (vMax - vMin < 4.0f) {
    float meio = roundf((vMax + vMin) / 2.0f);
    vMin = meio - 2.0f;
    vMax = meio + 2.0f;
  }
  char txt[8];
  snprintf(txt, sizeof(txt), "%.0f", vMax);
  c.setCursor(gx - 4 - (int16_t)strlen(txt) * 6, gy);
  c.print(txt);
  snprintf(txt, sizeof(txt), "%.0f", vMin);
  c.setCursor(gx - 4 - (int16_t)strlen(txt) * 6, gy + gh - 7);
  c.print(txt);
  // linha tracejada do limite do alarme (se estiver dentro da faixa)
  if (LIMITE_ALARME >= vMin && LIMITE_ALARME <= vMax) {
    int16_t y = gy + gh - (int16_t)((LIMITE_ALARME - vMin) / (vMax - vMin) * gh + 0.5f);
    for (int16_t x = gx; x < gx + gw; x += 4) {
      c.drawFastHLine(x, y, 2, COR_VERMELHO);
    }
  }
  // a linha da temperatura (ponto mais novo à direita)
  int16_t xAnt = -1, yAnt = -1;
  int16_t inicio = gx + gw - nHistorico;
  for (int i = 0; i < nHistorico; i++) {
    float v = historico[i];
    if (isnan(v)) {
      xAnt = -1;   // buraco no gráfico (sensor com problema)
      continue;
    }
    int16_t x = inicio + i;
    int16_t y = gy + gh - (int16_t)((v - vMin) / (vMax - vMin) * gh + 0.5f);
    if (xAnt >= 0) {
      c.drawLine(xAnt, yAnt, x, y, corLinha);
    } else {
      c.drawPixel(x, y, corLinha);
    }
    xAnt = x;
    yAnt = y;
  }
  if (xAnt >= 0) {
    c.fillCircle(xAnt, yAnt, 2, corLinha);   // destaca o ponto atual
  }
}

void desenharTela(GFXcanvas16 &c, uint32_t agora) {
  char txt[32];
  int16_t dx;
  bool pisca = ((agora / 500) % 2) == 0;
  uint16_t corTemp = !dhtPronto ? COR_CINZA : (!dhtOk ? COR_AMARELO : (alarmeTemp ? COR_VERMELHO : COR_VERDE));
  bool tempPiscando = alarmeTemp && pisca;
  bool vibPiscando = alarmeVib && pisca;

  c.fillScreen(COR_FUNDO);

  // ---------- Barra de cima: motor e tempo ligado ----------
  c.fillRect(0, 0, LARGURA_TELA, 13, COR_BARRA);
  c.setFont(nullptr);
  c.setTextSize(1);
  c.setTextColor(COR_TEXTO);
  c.setCursor(4, 3);
  snprintf(txt, sizeof(txt), "MOTOR %02u", (unsigned)ID_MOTOR);
  c.print(txt);
  char ligado[12];
  formatarLigado(ligado, sizeof(ligado), agora / 1000);
  snprintf(txt, sizeof(txt), "LIGADO %s", ligado);
  c.setTextColor(COR_TEXTO_FRACO);
  c.setCursor(LARGURA_TELA - 4 - (int16_t)strlen(txt) * 6, 3);
  c.print(txt);

  // ---------- Bloco da temperatura ----------
  const int16_t yBloco = 16, altBloco = 44;
  uint16_t corValor = corTemp;
  uint16_t corRotulo = COR_TEXTO_FRACO;
  if (tempPiscando) {
    c.fillRect(0, yBloco, LARGURA_TELA, altBloco, COR_VERMELHO);
    corValor = COR_TEXTO;
    corRotulo = COR_TEXTO;
  }

  c.setTextColor(corRotulo);
  c.setCursor(6, yBloco + 4);
  c.print("TEMPERATURA");

  // "pílula" com a situação (alarmes, erro do sensor ou normal)
  const char *st = textoSituacao();
  int16_t wPilula = strlen(st) * 6 + 8;
  int16_t xPilula = LARGURA_TELA - 6 - wPilula;
  if (tempPiscando) {
    c.fillRoundRect(xPilula, yBloco + 2, wPilula, 11, 3, COR_TEXTO);
    c.setTextColor(COR_VERMELHO);
  } else {
    c.fillRoundRect(xPilula, yBloco + 2, wPilula, 11, 3, corSituacao());
    c.setTextColor(COR_FUNDO);
  }
  c.setCursor(xPilula + 4, yBloco + 4);
  c.print(st);

  // valor grande
  if (dhtOk) {
    snprintf(txt, sizeof(txt), "%.1f", ultimaTemperatura);
  } else {
    snprintf(txt, sizeof(txt), "--.-");
  }
  c.setFont(&FreeSansBold18pt7b);
  c.setTextColor(corValor);
  uint16_t wValor = larguraTexto(c, txt, &dx);
  int16_t yValor = yBloco + 38;   // linha de base do número grande
  c.setCursor(6 - dx, yValor);
  c.print(txt);
  // "°C"
  int16_t xGrau = 6 + wValor + 6;
  c.drawCircle(xGrau, yValor - 21, 2, corValor);
  c.drawCircle(xGrau, yValor - 21, 3, corValor);
  c.setFont(&FreeSansBold9pt7b);
  c.setCursor(xGrau + 5, yValor - 11);
  c.print("C");

  // coluna da direita: umidade, máxima e mínima (ou avisos)
  c.setFont(nullptr);
  c.setTextColor(tempPiscando ? COR_TEXTO : COR_TEXTO_FRACO);
  char linhas[3][20];
  int nLinhas = 0;
  if (!dhtPronto) {
    snprintf(linhas[nLinhas++], sizeof(linhas[0]), "LENDO O");
    snprintf(linhas[nLinhas++], sizeof(linhas[0]), "SENSOR...");
  } else if (!dhtOk) {
    snprintf(linhas[nLinhas++], sizeof(linhas[0]), "VER FIOS");
    snprintf(linhas[nLinhas++], sizeof(linhas[0]), "DO SENSOR");
  } else {
    if (!isnan(ultimaUmidade)) {
      snprintf(linhas[nLinhas++], sizeof(linhas[0]), "UMID %.0f%%", ultimaUmidade);
    }
    if (temMinMax) {
      snprintf(linhas[nLinhas++], sizeof(linhas[0]), "MAX %.1f", tempMax);
      snprintf(linhas[nLinhas++], sizeof(linhas[0]), "MIN %.1f", tempMin);
    }
  }
  int16_t yLinha = (nLinhas == 3) ? yBloco + 16 : yBloco + 18;
  int16_t passoLinha = (nLinhas == 3) ? 9 : 11;
  for (int i = 0; i < nLinhas; i++) {
    c.setCursor(LARGURA_TELA - 6 - (int16_t)strlen(linhas[i]) * 6, yLinha + i * passoLinha);
    c.print(linhas[i]);
  }

  // ---------- Linha da vibração (MPU6050) ----------
  const int16_t yVib = yBloco + altBloco + 1, altVib = 13;
  if (vibPiscando) {
    c.fillRect(0, yVib, LARGURA_TELA, altVib, COR_VERMELHO);
  }
  c.setTextColor(vibPiscando ? COR_TEXTO : COR_TEXTO_FRACO);
  c.setCursor(6, yVib + 3);
  if (!mpuOk) {
    c.print("VIB");
    c.setTextColor(COR_AMARELO);
    c.setCursor(6 + 5 * 6, yVib + 3);
    c.print("SEM SENSOR");
  } else if (isnan(vibracaoAtual)) {
    c.print("VIB --");
  } else {
    snprintf(txt, sizeof(txt), "VIB %.2fg", vibracaoAtual);
    c.print(txt);
    // barra: cheia = 1,5 vez o limite; o tracinho branco marca o limite
    const int16_t xBarra = 70, wBarra = LARGURA_TELA - 6 - xBarra, yBarra = yVib + 3, hBarra = 7;
    float fracao = vibracaoAtual / (LIMITE_VIBRACAO * 1.5f);
    if (fracao > 1.0f) fracao = 1.0f;
    int16_t wCheio = (int16_t)(fracao * wBarra + 0.5f);
    uint16_t corBarra = (vibracaoAtual >= LIMITE_VIBRACAO) ? COR_VERMELHO : COR_VERDE;
    if (vibPiscando) corBarra = COR_TEXTO;
    c.fillRect(xBarra, yBarra, wBarra, hBarra, vibPiscando ? COR_FUNDO : COR_LINHA);
    c.fillRect(xBarra, yBarra, wCheio, hBarra, corBarra);
    int16_t xLimite = xBarra + (int16_t)(wBarra / 1.5f + 0.5f);
    c.drawFastVLine(xLimite, yBarra - 2, hBarra + 4, COR_TEXTO);
  }

  // ---------- Gráfico da temperatura ----------
  const int16_t yGrafico = yVib + altVib + 4;
  desenharGrafico(c, 4, yGrafico, LARGURA_TELA - 10, ALTURA_TELA - yGrafico - 14, dhtOk ? corTemp : COR_CINZA);

  // ---------- Rodapé: limites dos alarmes ----------
  c.setFont(nullptr);
  c.setCursor(4, ALTURA_TELA - 9);
  if (ultimoEnvioOk) {
    char lim[8];
    formatarGraus(lim, sizeof(lim), LIMITE_ALARME);
    snprintf(txt, sizeof(txt), "LIMITES: %sC e %.2fg", lim, LIMITE_VIBRACAO);
    c.setTextColor(COR_TEXTO_FRACO);
  } else {
    snprintf(txt, sizeof(txt), "ERRO NO RADIO");
    c.setTextColor(COR_VERMELHO);
  }
  c.print(txt);
}

// ======================= RÁDIO (ESP-NOW) =======================

// Liga o rádio no modo ESP-NOW, no canal fixo, enviando para "todos" (broadcast).
void iniciarEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // Fixa o canal de rádio (a pulseira escuta no mesmo canal)
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(CANAL_ESPNOW, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ERRO: esp_now_init falhou. Reiniciando em 2 s...");
    delay(2000);
    ESP.restart();
  }

  esp_now_peer_info_t destino = {};
  memcpy(destino.peer_addr, ENDERECO_BROADCAST, 6);
  destino.channel = CANAL_ESPNOW;
  destino.ifidx   = WIFI_IF_STA;
  destino.encrypt = false;
  if (esp_now_add_peer(&destino) != ESP_OK) {
    Serial.println("ERRO: nao consegui registrar o destino broadcast");
  }
}

void piscarLed() {
  if (PINO_LED < 0) {
    return;
  }
  estadoLed = !estadoLed;
  digitalWrite(PINO_LED, estadoLed ? HIGH : LOW);
}

// ======================= MPU6050 (VIBRAÇÃO) =======================

bool escreverMpu(uint8_t registrador, uint8_t valor) {
  Wire.beginTransmission(enderecoMpu);
  Wire.write(registrador);
  Wire.write(valor);
  return Wire.endTransmission() == 0;
}

// Confere o I2C antes de usar. Se o ESP reiniciou no meio de uma leitura, o MPU6050
// pode ficar segurando o SDA em 0: até 9 pulsos no SCL fazem ele soltar.
void destravarI2C(int sda, int scl) {
  pinMode(sda, INPUT_PULLDOWN);   // com o módulo ligado, os resistores dele puxam para 1
  pinMode(scl, INPUT_PULLDOWN);
  delayMicroseconds(20);
  bool sdaAlto = digitalRead(sda), sclAlto = digitalRead(scl);
  if (sdaAlto && sclAlto) return;   // tudo certo
  if (!sclAlto) {
    Serial.println("I2C sem sinal no SCL: modulo sem energia ou fio solto");
    return;
  }
  Serial.println("I2C travado (SDA preso em 0): destravando...");
  pinMode(scl, OUTPUT_OPEN_DRAIN);
  for (int i = 0; i < 9 && digitalRead(sda) == LOW; i++) {
    digitalWrite(scl, LOW);
    delayMicroseconds(5);
    digitalWrite(scl, HIGH);
    delayMicroseconds(5);
  }
  pinMode(sda, OUTPUT_OPEN_DRAIN);   // sinal de "fim de conversa": SDA sobe com o SCL em 1
  digitalWrite(sda, LOW);
  delayMicroseconds(5);
  digitalWrite(sda, HIGH);
  delayMicroseconds(5);
}

// Procura o MPU6050 e liga o acelerômetro: escala de ±4 g, filtro de 184 Hz
bool iniciarMpu() {
  destravarI2C(MPU_SDA, MPU_SCL);
  Wire.begin(MPU_SDA, MPU_SCL, 400000);
  enderecoMpu = 0;
  const uint8_t enderecos[2] = {0x68, 0x69};   // 0x69 só se o AD0 estiver em 3V3
  for (int i = 0; i < 2 && enderecoMpu == 0; i++) {
    Wire.beginTransmission(enderecos[i]);
    if (Wire.endTransmission() == 0) {
      enderecoMpu = enderecos[i];
    }
  }
  if (enderecoMpu == 0) {
    Serial.println("MPU6050 NAO respondeu (confira SDA->D33, SCL->D32, VCC->3V3, GND->GND)");
    return false;
  }
  bool ok = escreverMpu(0x6B, 0x01);   // PWR_MGMT_1: acorda o sensor
  delay(50);
  ok = ok && escreverMpu(0x19, 0x00)   // SMPLRT_DIV: 1000 medidas por segundo
          && escreverMpu(0x1A, 0x01)   // CONFIG: filtro de 184 Hz
          && escreverMpu(0x1C, 0x08);  // ACCEL_CONFIG: escala de ±4 g (8192 = 1 g)
  if (!ok) {
    Serial.println("MPU6050 respondeu, mas nao aceitou a configuracao");
    enderecoMpu = 0;
    return false;
  }
  Serial.printf("MPU6050 encontrado (0x%02X)\n", enderecoMpu);
  return true;
}

// Lê a aceleração dos 3 eixos (valores brutos: 8192 = 1 g)
bool lerAcelerometro(int16_t &ax, int16_t &ay, int16_t &az) {
  Wire.beginTransmission(enderecoMpu);
  Wire.write(0x3B);   // ACCEL_XOUT_H: os 6 bytes seguintes são X, Y e Z
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)enderecoMpu, 6) != 6) return false;
  uint8_t b[6];
  for (int i = 0; i < 6; i++) {
    b[i] = Wire.read();
  }
  ax = (int16_t)((b[0] << 8) | b[1]);
  ay = (int16_t)((b[2] << 8) | b[3]);
  az = (int16_t)((b[4] << 8) | b[5]);
  return true;
}

// Guarda uma medida a cada 2 ms (chamada o tempo todo pelo loop)
void amostrarVibracao() {
  if (enderecoMpu == 0) return;
  uint32_t agoraUs = micros();
  if ((int32_t)(agoraUs - proximaAmostraUs) < 0) return;
  proximaAmostraUs = agoraUs + AMOSTRA_US;
  int16_t ax, ay, az;
  if (!lerAcelerometro(ax, ay, az)) {
    proximaAmostraUs = agoraUs + 100000;   // não respondeu: espera 0,1 s antes de tentar de novo
    return;
  }
  somaX += ax;
  somaY += ay;
  somaZ += az;
  somaQX += (int32_t)ax * ax;
  somaQY += (int32_t)ay * ay;
  somaQZ += (int32_t)az * az;
  nAmostras++;
}

// Quanto um eixo variou em volta da média (a média é a gravidade, que não conta)
double variancia(int32_t soma, int64_t somaQ, uint32_t n) {
  double media = (double)soma / n;
  return (double)somaQ / n - media * media;
}

// A cada 1 s: vibração = o quanto a aceleração variou no segundo, em g
void fecharJanelaVibracao() {
  mpuOk = nAmostras >= 100;   // pelo menos 100 das ~500 medidas do segundo
  if (mpuOk) {
    double v = variancia(somaX, somaQX, nAmostras) + variancia(somaY, somaQY, nAmostras) +
               variancia(somaZ, somaQZ, nAmostras);
    vibracaoAtual = sqrt(v > 0 ? v : 0) / 8192.0;
    janelasSemMpu = 0;
  } else {
    vibracaoAtual = NAN;
    // sem resposta: tenta achar o MPU6050 de novo a cada 5 s (fio solto, ligado depois...)
    if (++janelasSemMpu >= 5) {
      janelasSemMpu = 0;
      iniciarMpu();
    }
  }
  somaX = somaY = somaZ = 0;
  somaQX = somaQY = somaQZ = 0;
  nAmostras = 0;

  // Alarme: liga depois de 2 s acima do limite e desliga depois de 3 s abaixo
  // de 80% do limite. Assim uma batida na máquina não dispara o alarme.
  if (!mpuOk) {
    alarmeVib = false;
    segundosAcima = segundosAbaixo = 0;
  } else if (vibracaoAtual >= LIMITE_VIBRACAO) {
    segundosAbaixo = 0;
    if (segundosAcima < 255) segundosAcima++;
    if (segundosAcima >= SEGUNDOS_PARA_LIGAR) alarmeVib = true;
  } else if (vibracaoAtual < LIMITE_VIBRACAO * 0.8f) {
    segundosAcima = 0;
    if (segundosAbaixo < 255) segundosAbaixo++;
    if (segundosAbaixo >= SEGUNDOS_PARA_DESLIGAR) alarmeVib = false;
  } else {
    segundosAcima = segundosAbaixo = 0;   // entre 80% e 100% do limite: fica como está
  }
}

// ======================= TEMPERATURA (DHT11) =======================

void lerTemperatura() {
  // Lê o sensor (força uma leitura nova; a umidade sai da mesma leitura)
  float temperatura = sensor.readTemperature(false, true);
  float umidade = sensor.readHumidity(false);
  bool leituraOk = !isnan(temperatura) && !isnan(umidade);

  if (leituraOk) {
    falhasSeguidas = 0;
    ultimaTemperatura = temperatura;
    ultimaUmidade = umidade;
    if (!temMinMax) {
      tempMin = tempMax = temperatura;
      temMinMax = true;
    }
    if (temperatura < tempMin) tempMin = temperatura;
    if (temperatura > tempMax) tempMax = temperatura;
  } else if (falhasSeguidas < 255) {
    falhasSeguidas++;
  }
  leituraFalhou = !leituraOk;

  // Uma falha de vez em quando é normal no DHT11: repete a última leitura boa.
  // Só avisa "sensor com problema" depois de FALHAS_ATE_ERRO falhas seguidas.
  bool temLeitura = !isnan(ultimaTemperatura);
  dhtOk = temLeitura && falhasSeguidas < FALHAS_ATE_ERRO;
  dhtPronto = temLeitura || falhasSeguidas >= FALHAS_ATE_ERRO;

  // Alarme: liga no limite e só desliga 1 °C abaixo (histerese)
  if (!dhtOk) {
    alarmeTemp = false;
  } else if (ultimaTemperatura >= LIMITE_ALARME) {
    alarmeTemp = true;
  } else if (ultimaTemperatura <= LIMITE_ALARME - HISTERESE) {
    alarmeTemp = false;
  }
}

// ======================= ENVIO PARA A PULSEIRA =======================

void avisarMudancaSituacao() {
  const char *situacao = textoSituacao();
  if (strcmp(situacao, ultimaSituacao) != 0) {
    Serial.printf(">>> Situacao: %s -> %s\n", ultimaSituacao[0] ? ultimaSituacao : "-", situacao);
    ultimaSituacao = situacao;
  }
}

void enviarPacote(uint32_t agora) {
  if (!dhtPronto) {
    Serial.println("Esperando a primeira leitura do DHT11...");
    return;   // ainda não manda nada para a pulseira
  }

  PacoteMotor pacote = {};
  pacote.magico          = PACOTE_MAGICO;
  pacote.versao          = PACOTE_VERSAO;
  pacote.idMotor         = ID_MOTOR;
  pacote.sensorOk        = dhtOk ? 1 : 0;
  pacote.umidade         = dhtOk ? (uint8_t)constrain(lroundf(ultimaUmidade), 0, 100) : 255;
  pacote.sequencia       = ++sequencia;
  pacote.temperaturaC    = dhtOk ? ultimaTemperatura : NAN;
  pacote.ligadoHaSeg     = agora / 1000;
  pacote.limiteC         = LIMITE_ALARME;
  pacote.vibracaoG       = mpuOk ? vibracaoAtual : NAN;
  pacote.limiteVibracaoG = LIMITE_VIBRACAO;
  pacote.alarme          = alarmeTemp ? 1 : 0;
  pacote.vibracaoOk      = mpuOk ? 1 : 0;
  pacote.alarmeVibracao  = alarmeVib ? 1 : 0;

  esp_err_t resultado = esp_now_send(ENDERECO_BROADCAST, (const uint8_t *)&pacote, sizeof(pacote));
  ultimoEnvioOk = (resultado == ESP_OK);

  // Mostra no Monitor Serial
  Serial.printf("#%lu  ", (unsigned long)pacote.sequencia);
  if (dhtOk) {
    Serial.printf("%.1f C  %.0f %%", ultimaTemperatura, ultimaUmidade);
    if (leituraFalhou) {
      Serial.print(" (leitura falhou, repetindo a anterior)");
      leituraFalhou = false;
    }
  } else {
    Serial.print("DHT11 COM PROBLEMA (confira S->D4, meio->3V3, - ->GND)");
  }
  if (mpuOk) {
    Serial.printf("  vib %.2f g", vibracaoAtual);
  } else {
    Serial.print("  vib --");
  }
  if (alarmeTemp) Serial.print("  ALARME TEMP");
  if (alarmeVib) Serial.print("  ALARME VIB");
  Serial.println(ultimoEnvioOk ? "  -> enviado" : "  -> FALHA ao enviar");

  piscarLed();
}

void atualizarHistorico(uint32_t agora) {
  if (!dhtPronto || agora - ultimoPontoHist < INTERVALO_HIST_MS) return;
  ultimoPontoHist = agora;
  float v = dhtOk ? ultimaTemperatura : NAN;
  if (nHistorico < TAM_HISTORICO) {
    historico[nHistorico++] = v;
  } else {
    memmove(historico, historico + 1, (TAM_HISTORICO - 1) * sizeof(float));
    historico[TAM_HISTORICO - 1] = v;
  }
  precisaRedesenhar = true;
}

// ======================= SETUP E LOOP =======================

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("===== ESP2 - MOTOR (DHT11 + MPU6050 + TFT) =====");

  if (PINO_LED >= 0) {
    pinMode(PINO_LED, OUTPUT);
  }

  // Tela
  tft.initR(TIPO_TELA);
  tft.setRotation(ROTACAO_TELA);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(8, 12);
  tft.print("MOTOR: INICIANDO...");
  tela = new GFXcanvas16(LARGURA_TELA, ALTURA_TELA);
  if (tela == nullptr || tela->getBuffer() == nullptr) {
    Serial.println("ERRO: sem memoria para a tela");
  }

  sensor.begin();   // já liga o pull-up interno do ESP32 no pino do sensor
  mpuOk = iniciarMpu();
  delay(1500);      // o DHT11 precisa de ~1 s depois de ligar

  iniciarEspNow();
  Serial.print("MAC deste ESP32: ");
  Serial.println(WiFi.macAddress());
  Serial.printf("Motor %d enviando no canal %d a cada %d ms\n", ID_MOTOR, CANAL_ESPNOW, JANELA_MS);
  Serial.printf("Alarmes: temperatura a partir de %.1f C, vibracao a partir de %.2f g\n",
                LIMITE_ALARME, LIMITE_VIBRACAO);

  inicioJanela = millis();
  ultimaLeituraDht = millis() - INTERVALO_DHT_MS;   // lê o DHT11 já na primeira volta
}

void loop() {
  uint32_t agora = millis();

  // 1) Vibração: guarda uma medida a cada 2 ms
  amostrarVibracao();

  // 2) Temperatura: a cada 2 s
  if (agora - ultimaLeituraDht >= INTERVALO_DHT_MS) {
    ultimaLeituraDht = agora;
    lerTemperatura();
  }

  // 3) A cada 1 s: calcula a vibração, decide os alarmes e envia para a pulseira
  if (agora - inicioJanela >= JANELA_MS) {
    inicioJanela = agora;
    fecharJanelaVibracao();
    avisarMudancaSituacao();
    enviarPacote(agora);
    precisaRedesenhar = true;
  }

  atualizarHistorico(agora);

  // 4) Redesenha: dado novo, mudou o segundo ("ligado há") ou um alarme pisca
  bool pisca = ((agora / 500) % 2) == 0;
  bool mudouPisca = (alarmeTemp || alarmeVib) && (pisca != ultimoPisca);
  uint32_t segundo = agora / 1000;
  if (precisaRedesenhar || mudouPisca || segundo != ultimoSegundoDesenhado) {
    ultimoSegundoDesenhado = segundo;
    ultimoPisca = pisca;
    precisaRedesenhar = false;
    if (tela != nullptr && tela->getBuffer() != nullptr) {
      desenharTela(*tela, agora);
      tft.drawRGBBitmap(0, 0, tela->getBuffer(), LARGURA_TELA, ALTURA_TELA);
    }
  }
}
