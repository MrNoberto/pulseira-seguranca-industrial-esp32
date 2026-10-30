/*
  ============================================================
   ESP1 - PULSEIRA  -  Projeto "Pulseira Monitor de Motor"
   Placa: ESP32 DevKit (o "tradicional", 30 ou 38 pinos)
   Autor: Noberto
  ============================================================

  O que este código faz:
    - Recebe do motor (ESP2), a cada 1 segundo, a temperatura, a
      vibração e os avisos de alarme, pelo ESP-NOW (o rádio do próprio
      ESP32, sem roteador e sem internet).
    - Mede a força do sinal do ESP3 (zona de perigo): quanto mais forte,
      mais perto. Chegou mais perto que a distância escolhida = alarme.
    - Mostra na tela OLED 0,96": a temperatura do motor, a distância até
      a zona, a força do sinal do motor e a situação.
    - Quando há alarme, o buzzer bipa sem parar:
        3 bipes curtos          = temperatura alta
        2 bipes longos          = vibração alta
        5 bipes rápidos, agudos = dentro da zona de perigo
      (com mais de um alarme, toca um de cada vez, em rodízio)
    - Botão BOOT da pulseira:
        toque rápido ... troca a distância da zona: 1 -> 2 -> 3 -> 5 m
        segurar 3 s .... calibra: fique parado NA distância escolhida, de
                         frente para o ESP3, mexendo a pulseira devagar.
                         A borda da zona fica exatamente ali.
      As duas ficam salvas, mesmo desligando.
    - Não marca hora. Os limites dos alarmes do motor ficam no código do ESP2.
    - O LED azul da placa pisca a cada mensagem do motor.
    - 2 LEDs de situação (filamentos de 3 V):
        azul (D25) ...... aceso = tudo perfeito
        amarelo (D26) ... pisca rápido = perigo (temperatura, vibração ou zona de perigo)
                          pisca devagar = algo errado (sensor com problema ou motor sem sinal)
      Ao ligar, os LEDs acendem por meio segundo (teste das ligações).
      (Dá para usar 4 LEDs, um par para o motor e outro para a zona: veja QUANTIDADE_LEDS.)

  Ligações:
    Tela OLED 0,96" (I2C)   VCC -> 3V3   GND -> GND   SCL -> D22   SDA -> D21
    Buzzer passivo KY-006   S -> D32     meio -> não liga   - -> GND
    LEDs (filamento 3 V)    + -> D25 (azul), D26 (amarelo)    - -> GND
                            (sem resistor: o código limita a corrente do pino)
                            com 4 LEDs: azul 2 no D27 e amarelo 2 no D33

  Biblioteca (Arduino IDE > Gerenciador de Bibliotecas):
    - U8g2   (de oliver)
  Placa: "ESP32 Dev Module" (pacote esp32 da Espressif, versão 3.x)
*/

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include "driver/gpio.h"

#if !defined(ESP_ARDUINO_VERSION_MAJOR) || ESP_ARDUINO_VERSION_MAJOR < 3
#error "Atualize o pacote de placas esp32 para a versao 3.x (Ferramentas > Placa > Gerenciador de Placas)"
#endif

// ======================= CONFIGURAÇÕES =======================

const uint32_t TEMPO_SEM_SINAL_MS = 10000;  // 10 s sem pacote do motor = leitura velha, não usa mais
const uint8_t  ID_MOTOR_ESPERADO  = 0;      // 0 = aceita qualquer motor | 1..255 = só esse
const bool     BIP_AO_LIGAR       = true;   // bip curto ao ligar = buzzer funcionando
#define CANAL_ESPNOW 1                      // tem que ser IGUAL no ESP2 (motor) e no ESP3 (zona)

// Zona de perigo (ESP3). A distância vem da força do sinal: é aproximada
// (paredes e o próprio corpo mudam o sinal). Calibre no lugar de uso.
const float    DISTANCIAS_M[]      = {1.0, 2.0, 3.0, 5.0};   // opções do toque rápido no BOOT
const float    DISTANCIA_PADRAO_M  = 2.0;     // distância antes de mexer no botão
const float    RSSI_1M_PADRAO      = -60.0;   // força do sinal a 1 m antes de calibrar (dBm)
const float    N_AMBIENTE          = 2.5;     // quanto o sinal cai com a distância (2 = aberto, 3 = muitas paredes)
const float    HISTERESE_ZONA_DB   = 3.0;     // para sair da zona, precisa se afastar um pouco mais
const uint32_t TEMPO_SEM_ZONA_MS   = 3000;    // 3 s sem o sinal do ESP3 = zona fora de alcance

#define PINO_BUZZER 32   // buzzer PASSIVO (KY-006), pino S
#define FREQ_BIP    2500 // tom do bipe em Hz (som fraco? teste outro valor entre 2000 e 4000)
#define FREQ_ZONA   3500 // tom do bipe da zona (mais agudo, para diferenciar)
#define PINO_LED    2    // LED azul da placa (DevKit de 30 pinos). Se a sua não tiver, tudo bem
#define PINO_BOTAO  0    // botão BOOT da placa

// LEDs de situação (filamentos de 3 V / 100 mA). O pino do ESP32 não aguenta 100 mA:
// o código deixa estes pinos numa força média (~15 mA com esses LEDs de ~2,8 V), o máximo
// sem forçar o pino. Acende como um LED de sinalização forte.
#define QUANTIDADE_LEDS     2    // 2 (azul no D25, amarelo no D26) ou 4 (par do motor + par da zona)
#define PINO_AZUL_MOTOR     25   // com 2 LEDs: azul (tudo perfeito) | com 4: azul do motor
#define PINO_AMARELO_MOTOR  26   // com 2 LEDs: amarelo (perigo/problema) | com 4: amarelo do motor
#define PINO_AZUL_ZONA      27   // só com 4 LEDs: fora da zona de perigo (zona segura)
#define PINO_AMARELO_ZONA   33   // só com 4 LEDs: dentro da zona de perigo

// Tela OLED 0,96" (SSD1306, I2C)
#define OLED_SDA 21
#define OLED_SCL 22

// ============ PACOTE DO MOTOR (tem que ser IGUAL no ESP2) ============
#define PACOTE_MAGICO 0x4E425431UL   // "NBT1"
#define PACOTE_VERSAO 4              // versão 4 = com a vibração do MPU6050

typedef struct __attribute__((packed)) {
  uint32_t magico;
  uint8_t  versao;
  uint8_t  idMotor;
  uint8_t  sensorOk;        // 1 = DHT11 com leitura válida
  uint8_t  umidade;         // % (255 = sem leitura). A pulseira não usa.
  uint32_t sequencia;
  float    temperaturaC;
  uint32_t ligadoHaSeg;
  float    limiteC;         // limite de temperatura escolhido no ESP2
  float    vibracaoG;       // vibração do último segundo, em g
  float    limiteVibracaoG; // limite de vibração escolhido no ESP2
  uint8_t  alarme;          // 1 = temperatura muito alta
  uint8_t  vibracaoOk;      // 1 = MPU6050 respondendo
  uint8_t  alarmeVibracao;  // 1 = vibração muito alta
} PacoteMotor;

// ============ PACOTE DO FAROL DA ZONA (tem que ser IGUAL no ESP3) ============
#define ZONA_MAGICO 0x4E425A31UL   // "NBZ1"
#define ZONA_VERSAO 1

typedef struct __attribute__((packed)) {
  uint32_t magico;
  uint8_t  versao;
  uint8_t  idZona;
  uint32_t sequencia;
} PacoteZona;

enum EstadoPulseira : uint8_t {
  EST_AGUARDANDO,    // ainda não chegou nenhum pacote do motor
  EST_NORMAL,        // sem alarme: silêncio
  EST_ALARME,        // temperatura, vibração ou zona: bipa
  EST_SEM_SINAL,     // parou de chegar pacote do motor
  EST_ERRO_SENSOR    // chega pacote, mas o DHT11 do motor está com problema
};

// ======================= OBJETOS E VARIÁVEIS =======================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
Preferences memoria;   // guarda a distância e a calibração da zona

// Recebidos pelo rádio (escritos na função do ESP-NOW, lidos no loop)
portMUX_TYPE travaPacote = portMUX_INITIALIZER_UNLOCKED;
volatile bool chegouPacote = false;
PacoteMotor pacoteNovo;
int rssiNovo = 0;
int32_t  somaRssiZona = 0;   // soma da força dos sinais do ESP3 (sempre mexida dentro da trava)
uint16_t nRssiZona = 0;

// Estado do motor
bool     jaRecebeu = false;
uint32_t momentoUltimoPacote = 0;
float    temperaturaAtual = NAN;
float    vibracaoAtual = NAN;
bool     sensorOkAtual = false;
bool     vibOkAtual = false;
bool     alarmeTempAtual = false;   // como veio no último pacote
bool     alarmeVibAtual = false;
bool     alarmeTempAtivo = false;   // só vale com dados frescos do motor
bool     alarmeVibAtivo = false;
uint8_t  idMotorAtual = 0;
int      rssiAtual = 0;
uint32_t pacotesPerdidos = 0, ultimaSequencia = 0;
EstadoPulseira estadoAtual = EST_AGUARDANDO;

// Zona de perigo
float    distanciaPerigo = DISTANCIA_PADRAO_M;
float    rssi1m = RSSI_1M_PADRAO;
bool     calibrado = false;
float    rssiZona = NAN;            // força média do sinal do ESP3 (dBm)
bool     zonaVista = false;         // recebendo o sinal do ESP3
bool     naZona = false;            // mais perto que a distância de perigo
uint32_t momentoUltimaZona = 0;
uint32_t ultimaMediaZona = 0;
uint32_t ultimoResumoZona = 0;

// Botão BOOT
bool     botaoAntes = false;
uint32_t inicioBotao = 0;
bool     acaoLongaFeita = false;
float    somaCalib = 0;             // força do sinal enquanto o botão está apertado (calibração)
uint16_t nCalib = 0;

// Buzzer (sem travar o código)
uint8_t  bipsRestantes = 0;
uint16_t bipLigadoMs = 100, bipDesligadoMs = 100;
uint16_t freqBip = FREQ_BIP;
bool     buzzerLigado = false;
uint32_t proximaTrocaBuzzer = 0;
uint8_t  ultimoAlarmeTocado = 2;    // 0 = temperatura, 1 = vibração, 2 = zona (rodízio)

// LED: fica aceso até este momento (piscada curta a cada mensagem do motor)
uint32_t apagarLedEm = 0;

// Tela
bool     telaOk = false;
bool     precisaRedesenhar = true;
uint32_t ultimaFase = 0xFFFFFFFF;
char     mensagem[24] = "";         // aviso rápido na linha de baixo (botão BOOT)
uint32_t mensagemAte = 0;

const char *textoDoEstado(EstadoPulseira e) {
  switch (e) {
    case EST_NORMAL:      return "NORMAL";
    case EST_ALARME:      return "ALARME";
    case EST_SEM_SINAL:   return "SEM SINAL";
    case EST_ERRO_SENSOR: return "ERRO SENSOR";
    default:              return "AGUARDANDO";
  }
}

// ======================= RÁDIO (ESP-NOW) =======================

// Chamada pelo ESP32 quando chega um pacote (do motor ou da zona). Precisa ser rápida.
void aoReceberPacote(const esp_now_recv_info_t *info, const uint8_t *dados, int tamanho) {
  int rssi = (info != nullptr && info->rx_ctrl != nullptr) ? info->rx_ctrl->rssi : 0;
  if (tamanho == (int)sizeof(PacoteZona)) {
    PacoteZona z;
    memcpy(&z, dados, sizeof(z));
    if (z.magico != ZONA_MAGICO || z.versao != ZONA_VERSAO) return;
    portENTER_CRITICAL(&travaPacote);
    somaRssiZona += rssi;
    nRssiZona++;
    portEXIT_CRITICAL(&travaPacote);
    return;
  }
  if (tamanho != (int)sizeof(PacoteMotor)) return;
  PacoteMotor p;
  memcpy(&p, dados, sizeof(p));
  if (p.magico != PACOTE_MAGICO || p.versao != PACOTE_VERSAO) return;
  if (ID_MOTOR_ESPERADO != 0 && p.idMotor != ID_MOTOR_ESPERADO) return;
  portENTER_CRITICAL(&travaPacote);
  pacoteNovo = p;
  rssiNovo = rssi;
  chegouPacote = true;
  portEXIT_CRITICAL(&travaPacote);
}

void iniciarEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(CANAL_ESPNOW, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ERRO: esp_now_init falhou. Reiniciando em 2 s...");
    delay(2000);
    ESP.restart();
  }
  esp_now_register_recv_cb(aoReceberPacote);
  Serial.printf("ESP-NOW ouvindo no canal %d\n", CANAL_ESPNOW);
}

// ======================= BUZZER E LED =======================

// O buzzer passivo só apita recebendo um tom: o pino liga e desliga freqBip vezes por segundo
void ligarSom() {
  ledcWriteTone(PINO_BUZZER, freqBip);
}

void desligarSom() {
  ledcWriteTone(PINO_BUZZER, 0);
}

void bipar(uint8_t vezes, uint16_t ligadoMs, uint16_t desligadoMs, uint16_t freq) {
  desligarSom();
  buzzerLigado = false;
  bipsRestantes = vezes;
  bipLigadoMs = ligadoMs;
  bipDesligadoMs = desligadoMs;
  freqBip = freq;
  proximaTrocaBuzzer = millis();
}

void calarBuzzer() {
  bipsRestantes = 0;
  buzzerLigado = false;
  desligarSom();
}

void atualizarBuzzer(uint32_t agora) {
  // alarmes: temperatura = 3 curtos | vibração = 2 longos | zona = 5 rápidos e agudos
  // com mais de um alarme, toca um de cada vez, em rodízio
  if (estadoAtual == EST_ALARME && bipsRestantes == 0 && !buzzerLigado &&
      (int32_t)(agora - proximaTrocaBuzzer) >= 0) {
    bool ativos[3] = {alarmeTempAtivo, alarmeVibAtivo, naZona};
    for (uint8_t k = 1; k <= 3; k++) {
      uint8_t i = (ultimoAlarmeTocado + k) % 3;
      if (!ativos[i]) continue;
      ultimoAlarmeTocado = i;
      if (i == 0) {
        bipar(3, 120, 90, FREQ_BIP);
      } else if (i == 1) {
        bipar(2, 350, 150, FREQ_BIP);
      } else {
        bipar(5, 60, 60, FREQ_ZONA);
      }
      break;
    }
  }
  if ((int32_t)(agora - proximaTrocaBuzzer) < 0) return;
  if (buzzerLigado) {
    desligarSom();
    buzzerLigado = false;
    proximaTrocaBuzzer = agora + (bipsRestantes > 0 ? bipDesligadoMs : 700);
  } else if (bipsRestantes > 0) {
    ligarSom();
    buzzerLigado = true;
    bipsRestantes--;
    proximaTrocaBuzzer = agora + bipLigadoMs;
  }
}

void atualizarLed(uint32_t agora) {
  bool aceso;
  if (estadoAtual == EST_ALARME) {
    aceso = buzzerLigado;                         // no alarme, pisca junto com o bipe
  } else {
    aceso = (int32_t)(apagarLedEm - agora) > 0;   // piscada curta a cada mensagem do motor
  }
  digitalWrite(PINO_LED, aceso ? HIGH : LOW);
}

// ======================= LEDS DE SITUAÇÃO =======================

const int PINOS_LEDS[4] = {PINO_AZUL_MOTOR, PINO_AMARELO_MOTOR, PINO_AZUL_ZONA, PINO_AMARELO_ZONA};

void iniciarLedsSituacao() {
  for (int i = 0; i < QUANTIDADE_LEDS; i++) {
    pinMode(PINOS_LEDS[i], OUTPUT);
    // força média no pino (~15 mA): brilho alto sem forçar o ESP32 e sem precisar de resistor
    gpio_set_drive_capability((gpio_num_t)PINOS_LEDS[i], GPIO_DRIVE_CAP_2);
    digitalWrite(PINOS_LEDS[i], HIGH);   // teste: todos acesos por meio segundo
  }
  delay(500);
  for (int i = 0; i < QUANTIDADE_LEDS; i++) {
    digitalWrite(PINOS_LEDS[i], LOW);
  }
}

// azul = tudo bem (aceso) | amarelo = perigo (pisca rápido) ou problema (pisca devagar)
void atualizarLedsSituacao(uint32_t agora) {
  bool piscaRapido = ((agora / 250) % 2) == 0;    // 2 vezes por segundo
  bool piscaDevagar = ((agora / 500) % 2) == 0;   // 1 vez por segundo
  bool fresco = jaRecebeu && agora - momentoUltimoPacote <= TEMPO_SEM_SINAL_MS;
  bool perigoMotor = alarmeTempAtivo || alarmeVibAtivo;
  bool motorOk = fresco && sensorOkAtual && !perigoMotor;
  if (QUANTIDADE_LEDS >= 4) {
    // sem dados do motor (procurando, sem sinal ou sensor com erro): azul 1 pisca devagar
    digitalWrite(PINO_AZUL_MOTOR, (motorOk || (!perigoMotor && piscaDevagar)) ? HIGH : LOW);
    digitalWrite(PINO_AMARELO_MOTOR, (perigoMotor && piscaRapido) ? HIGH : LOW);
    digitalWrite(PINO_AZUL_ZONA, !naZona ? HIGH : LOW);
    digitalWrite(PINO_AMARELO_ZONA, (naZona && piscaRapido) ? HIGH : LOW);
  } else {
    // com 2 LEDs: azul = tudo perfeito | amarelo rápido = perigo | amarelo devagar = problema
    bool perigo = perigoMotor || naZona;
    bool problema = !fresco || !sensorOkAtual || !vibOkAtual;   // motor sem sinal ou sensor falhando
    bool amarelo = perigo ? piscaRapido : (problema && piscaDevagar);
    digitalWrite(PINO_AZUL_MOTOR, (!perigo && !problema) ? HIGH : LOW);
    digitalWrite(PINO_AMARELO_MOTOR, amarelo ? HIGH : LOW);
  }
}

// ======================= ZONA DE PERIGO (ESP3) =======================

// Força do sinal na borda da zona: cai 10 x N_AMBIENTE dBm cada vez que a distância multiplica por 10
float limiteZona() {
  return rssi1m - 10.0f * N_AMBIENTE * log10f(distanciaPerigo);
}

// Distância aproximada até o ESP3, pela força do sinal
float distanciaEstimada() {
  return powf(10.0f, (rssi1m - rssiZona) / (10.0f * N_AMBIENTE));
}

// 2 -> "2m" | 1.84 -> "1.8m" | 12.3 -> "12m"
void formatarMetros(char *saida, size_t tam, float m) {
  if (m > 99.0f) m = 99.0f;
  if (m >= 9.95f || fabsf(m - roundf(m)) < 0.05f) {
    snprintf(saida, tam, "%.0fm", m);
  } else {
    snprintf(saida, tam, "%.1fm", m);
  }
}

void mostrarMensagem(const char *txt, uint32_t agora) {
  snprintf(mensagem, sizeof(mensagem), "%s", txt);
  mensagemAte = agora + 2500;
  precisaRedesenhar = true;
}

// A cada 0,2 s: média da força dos sinais do ESP3 e a decisão "dentro da zona"
void atualizarZona(uint32_t agora) {
  if (agora - ultimaMediaZona < 200) return;
  ultimaMediaZona = agora;
  int32_t soma;
  uint16_t n;
  portENTER_CRITICAL(&travaPacote);
  soma = somaRssiZona;
  n = nRssiZona;
  somaRssiZona = 0;
  nRssiZona = 0;
  portEXIT_CRITICAL(&travaPacote);

  if (n > 0) {
    float media = (float)soma / n;
    if (zonaVista && !isnan(rssiZona)) {
      // suaviza: sinal ficando mais forte (chegando perto) muda rápido; mais fraco (saindo), devagar
      float peso = (media > rssiZona) ? 0.5f : 0.3f;
      rssiZona = rssiZona * (1.0f - peso) + media * peso;
    } else {
      rssiZona = media;
    }
    if (botaoAntes) {   // botão apertado: guarda para a média da calibração
      somaCalib += media;
      nCalib++;
    }
    momentoUltimaZona = agora;
    zonaVista = true;
  } else if (zonaVista && agora - momentoUltimaZona > TEMPO_SEM_ZONA_MS) {
    zonaVista = false;
    rssiZona = NAN;
  }

  bool antes = naZona;
  if (!zonaVista) {
    naZona = false;
  } else if (rssiZona >= limiteZona()) {
    naZona = true;
  } else if (rssiZona < limiteZona() - HISTERESE_ZONA_DB) {
    naZona = false;
  }
  if (naZona != antes) {
    Serial.printf(">>> Zona: %s\n", naZona ? "ENTROU na zona de perigo" : "saiu da zona de perigo");
  }
  if (zonaVista && agora - ultimoResumoZona >= 2000) {
    ultimoResumoZona = agora;
    char d[8], p[8];
    formatarMetros(d, sizeof(d), distanciaEstimada());
    formatarMetros(p, sizeof(p), distanciaPerigo);
    Serial.printf("Zona: %.0f dBm, ~%s (perigo a %s, borda em %.0f dBm)\n", rssiZona, d, p, limiteZona());
  }
}

// Toque rápido no BOOT: próxima distância da lista
void trocarDistancia(uint32_t agora) {
  const int n = sizeof(DISTANCIAS_M) / sizeof(DISTANCIAS_M[0]);
  int atual = 0;
  for (int i = 0; i < n; i++) {
    if (fabsf(DISTANCIAS_M[i] - distanciaPerigo) < 0.01f) atual = i;
  }
  distanciaPerigo = DISTANCIAS_M[(atual + 1) % n];
  memoria.putFloat("dist", distanciaPerigo);
  char d[8], txt[24];
  formatarMetros(d, sizeof(d), distanciaPerigo);
  snprintf(txt, sizeof(txt), "ZONA DE %s", d);
  mostrarMensagem(txt, agora);
  bipar(1, 60, 0, FREQ_ZONA);
  Serial.printf("Distancia da zona de perigo: %s\n", d);
}

// Segurar o BOOT 3 s parado na distância escolhida: a borda da zona fica exatamente ali
void calibrarZona(uint32_t agora) {
  if (!zonaVista || isnan(rssiZona)) {
    mostrarMensagem("SEM SINAL DA ZONA", agora);
    bipar(1, 400, 0, FREQ_BIP);
    return;
  }
  float rssiAqui = (nCalib >= 3) ? somaCalib / nCalib : rssiZona;    // média dos 3 s com o botão apertado
  rssi1m = rssiAqui + 10.0f * N_AMBIENTE * log10f(distanciaPerigo);  // a mesma conta da borda, ao contrário
  calibrado = true;
  memoria.putFloat("rssi1m", rssi1m);
  char d[8], txt[24];
  formatarMetros(d, sizeof(d), distanciaPerigo);
  snprintf(txt, sizeof(txt), "CALIBRADO EM %s", d);
  mostrarMensagem(txt, agora);
  bipar(2, 80, 80, FREQ_ZONA);
  Serial.printf("Zona calibrada: borda de %s com %.1f dBm (equivale a %.1f dBm a 1 m)\n", d, rssiAqui, rssi1m);
}

// Botão BOOT: toque rápido troca a distância; segurar 3 s calibra a borda onde você está
void atualizarBotao(uint32_t agora) {
  bool apertado = digitalRead(PINO_BOTAO) == LOW;
  if (apertado && !botaoAntes) {
    inicioBotao = agora;
    acaoLongaFeita = false;
    somaCalib = 0;
    nCalib = 0;
  }
  if (apertado && !acaoLongaFeita && agora - inicioBotao >= 3000) {
    acaoLongaFeita = true;
    calibrarZona(agora);
  }
  if (!apertado && botaoAntes && !acaoLongaFeita && agora - inicioBotao >= 50) {
    trocarDistancia(agora);
  }
  botaoAntes = apertado;
}

// ======================= TELA OLED =======================

// Confere o I2C antes de usar. Se o ESP reiniciou no meio de uma conversa com a
// tela, ela pode ficar segurando o SDA em 0: até 9 pulsos no SCL fazem ela soltar.
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

// Procura a tela no I2C (endereço 0x3C; algumas vêm com 0x3D)
bool iniciarTela() {
  destravarI2C(OLED_SDA, OLED_SCL);
  Wire.begin(OLED_SDA, OLED_SCL);
  const uint8_t enderecos[2] = {0x3C, 0x3D};
  for (int i = 0; i < 2; i++) {
    Wire.beginTransmission(enderecos[i]);
    if (Wire.endTransmission() == 0) {
      oled.setI2CAddress(enderecos[i] * 2);   // a U8g2 usa o endereço "de 8 bits"
      oled.begin();
      oled.setFontMode(1);                    // letras sem fundo
      Serial.printf("Tela OLED encontrada (0x%02X)\n", enderecos[i]);
      return true;
    }
  }
  Serial.println("Tela OLED NAO respondeu (confira SDA->D21, SCL->D22, VCC->3V3, GND->GND)");
  return false;
}

void escreverCentralizado(const char *txt, int y) {
  oled.drawStr((128 - oled.getStrWidth(txt)) / 2, y, txt);
}

// Barrinhas de sinal (0 a 4) no canto superior direito
void desenharSinal(int rssi, bool ativo) {
  int barras = 0;
  if (ativo) {
    if (rssi >= -60)      barras = 4;
    else if (rssi >= -70) barras = 3;
    else if (rssi >= -80) barras = 2;
    else                  barras = 1;
  }
  for (int i = 0; i < 4; i++) {
    int alt = 3 + i * 2;
    int x = 113 + i * 4;
    if (i < barras) {
      oled.drawBox(x, 10 - alt, 3, alt);
    } else {
      oled.drawHLine(x, 9, 3);   // barra apagada: só o "chão"
    }
  }
}

void desenharTela(uint32_t agora) {
  if (!telaOk) return;
  char txt[24];
  bool pisca = ((agora / 500) % 2) == 0;
  oled.clearBuffer();
  oled.setDrawColor(1);

  // ---------- Cabeçalho: motor, distância até a zona e sinal do motor ----------
  oled.setFont(u8g2_font_6x10_tf);
  if (idMotorAtual > 0) {
    snprintf(txt, sizeof(txt), "MOTOR %02u", idMotorAtual);
  } else {
    snprintf(txt, sizeof(txt), "MOTOR --");
  }
  oled.drawStr(0, 10, txt);
  if (zonaVista && !isnan(rssiZona)) {
    char d[8];
    formatarMetros(d, sizeof(d), distanciaEstimada());
    snprintf(txt, sizeof(txt), "ZONA %s", d);
    int w = oled.getStrWidth(txt);
    int xZona = 110 - w;
    if (naZona) {   // dentro da zona: texto invertido
      oled.drawBox(xZona - 2, 0, w + 4, 12);
      oled.setDrawColor(0);
      oled.drawStr(xZona, 10, txt);
      oled.setDrawColor(1);
    } else {
      oled.drawStr(xZona, 10, txt);
    }
  }
  bool motorFresco = estadoAtual != EST_AGUARDANDO && estadoAtual != EST_SEM_SINAL &&
                     jaRecebeu && agora - momentoUltimoPacote <= TEMPO_SEM_SINAL_MS;
  desenharSinal(rssiAtual, motorFresco);
  oled.drawHLine(0, 13, 128);

  // ---------- Temperatura grande, centralizada ----------
  bool fresco = jaRecebeu && agora - momentoUltimoPacote <= TEMPO_SEM_SINAL_MS;
  bool temValor = !isnan(temperaturaAtual) && jaRecebeu && (!fresco || sensorOkAtual);
  if (temValor) {
    snprintf(txt, sizeof(txt), "%.1f", temperaturaAtual);
  } else {
    snprintf(txt, sizeof(txt), "--.-");
  }
  oled.setFont(u8g2_font_helvB12_tr);
  int wC = oled.getStrWidth("C");
  oled.setFont(u8g2_font_logisoso28_tn);
  int wNumero = oled.getStrWidth(txt);
  int x = (128 - (wNumero + 9 + wC)) / 2;   // número + "°" + "C"
  oled.drawStr(x, 48, txt);
  oled.drawCircle(x + wNumero + 4, 22, 2);  // o "°"
  oled.setFont(u8g2_font_helvB12_tr);
  oled.drawStr(x + wNumero + 9, 32, "C");

  // ---------- Linha de baixo: aviso do botão ou a situação ----------
  oled.setFont(u8g2_font_6x10_tf);
  if ((int32_t)(mensagemAte - agora) > 0) {
    escreverCentralizado(mensagem, 62);
  } else {
    switch (estadoAtual) {
      case EST_ALARME: {
        if (pisca) {   // faixa clara piscando
          oled.drawBox(0, 52, 128, 12);
          oled.setDrawColor(0);
        }
        const char *t;
        if (naZona && (alarmeTempAtivo || alarmeVibAtivo)) t = "ZONA + MOTOR!";
        else if (naZona)                                   t = "ZONA DE PERIGO!";
        else if (alarmeTempAtivo && alarmeVibAtivo)        t = "TEMP E VIBRACAO!";
        else if (alarmeTempAtivo)                          t = "TEMPERATURA ALTA!";
        else                                               t = "VIBRACAO ALTA!";
        escreverCentralizado(t, 62);
        oled.setDrawColor(1);
        break;
      }
      case EST_NORMAL:
        if (vibOkAtual) {
          snprintf(txt, sizeof(txt), "NORMAL  VIB %.2fg", vibracaoAtual);
        } else {
          snprintf(txt, sizeof(txt), "NORMAL  VIB --");
        }
        escreverCentralizado(txt, 62);
        break;
      case EST_SEM_SINAL: {
        uint32_t seg = (agora - momentoUltimoPacote) / 1000;
        if (seg < 60) {
          snprintf(txt, sizeof(txt), "SEM SINAL HA %lus", (unsigned long)seg);
        } else {
          snprintf(txt, sizeof(txt), "SEM SINAL HA %lumin", (unsigned long)(seg / 60));
        }
        escreverCentralizado(txt, 62);
        break;
      }
      case EST_ERRO_SENSOR:
        escreverCentralizado("ERRO SENSOR TEMP", 62);
        break;
      default:
        escreverCentralizado("PROCURANDO O MOTOR", 62);
        break;
    }
  }
  oled.sendBuffer();
}

// ======================= LÓGICA DO ALARME =======================

EstadoPulseira calcularEstado(uint32_t agora) {
  bool fresco = jaRecebeu && agora - momentoUltimoPacote <= TEMPO_SEM_SINAL_MS;
  alarmeTempAtivo = fresco && alarmeTempAtual;
  alarmeVibAtivo = fresco && alarmeVibAtual;
  if (alarmeTempAtivo || alarmeVibAtivo || naZona) return EST_ALARME;
  if (!jaRecebeu) return EST_AGUARDANDO;
  if (!fresco) return EST_SEM_SINAL;
  if (!sensorOkAtual) return EST_ERRO_SENSOR;
  return EST_NORMAL;
}

void aoMudarEstado(EstadoPulseira antigo, EstadoPulseira novo) {
  Serial.printf(">>> Estado: %s -> %s\n", textoDoEstado(antigo), textoDoEstado(novo));
  if (antigo == EST_ALARME) {
    calarBuzzer();   // saiu do alarme: para de bipar na hora
  }
}

void processarPacote(const PacoteMotor &p, int rssi, uint32_t agora) {
  if (jaRecebeu && p.sequencia > ultimaSequencia + 1 && p.sequencia - ultimaSequencia < 10000) {
    pacotesPerdidos += p.sequencia - ultimaSequencia - 1;
  }
  ultimaSequencia = p.sequencia;
  jaRecebeu = true;
  momentoUltimoPacote = agora;
  idMotorAtual = p.idMotor;
  rssiAtual = rssi;
  sensorOkAtual = (p.sensorOk == 1) && !isnan(p.temperaturaC);
  alarmeTempAtual = sensorOkAtual && p.alarme == 1;
  vibOkAtual = (p.vibracaoOk == 1) && !isnan(p.vibracaoG);
  alarmeVibAtual = vibOkAtual && p.alarmeVibracao == 1;
  vibracaoAtual = vibOkAtual ? p.vibracaoG : NAN;
  if (sensorOkAtual) {
    temperaturaAtual = p.temperaturaC;
  }
  apagarLedEm = agora + 60;

  Serial.printf("Motor %u  #%lu  ", p.idMotor, (unsigned long)p.sequencia);
  if (sensorOkAtual) {
    Serial.printf("%.1f C", p.temperaturaC);
  } else {
    Serial.print("SENSOR TEMP COM PROBLEMA");
  }
  if (vibOkAtual) {
    Serial.printf("  vib %.2f g", p.vibracaoG);
  } else {
    Serial.print("  vib --");
  }
  if (alarmeTempAtual) Serial.print("  ALARME TEMP");
  if (alarmeVibAtual) Serial.print("  ALARME VIB");
  Serial.printf("  sinal %d dBm  perdidos %lu\n", rssi, (unsigned long)pacotesPerdidos);
}

// ======================= SETUP E LOOP =======================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("===== ESP1 - PULSEIRA =====");

  ledcAttach(PINO_BUZZER, FREQ_BIP, 10);   // gerador de tom do ESP32 no pino do buzzer
  desligarSom();
  pinMode(PINO_LED, OUTPUT);
  digitalWrite(PINO_LED, LOW);
  pinMode(PINO_BOTAO, INPUT_PULLUP);
  iniciarLedsSituacao();

  // distância e calibração da zona guardadas na memória da placa
  memoria.begin("zona", false);
  distanciaPerigo = memoria.getFloat("dist", DISTANCIA_PADRAO_M);
  rssi1m = memoria.getFloat("rssi1m", RSSI_1M_PADRAO);
  calibrado = memoria.isKey("rssi1m");
  char d[8];
  formatarMetros(d, sizeof(d), distanciaPerigo);
  Serial.printf("Zona de perigo: %s, %s (%.0f dBm a 1 m)\n", d, calibrado ? "calibrada" : "SEM calibrar", rssi1m);

  telaOk = iniciarTela();
  iniciarEspNow();
  Serial.println("Esperando o ESP2 (motor) e o ESP3 (zona)...");

  if (BIP_AO_LIGAR) {
    bipar(1, 120, 0, FREQ_BIP);   // bip curto = buzzer funcionando
  }
}

void loop() {
  uint32_t agora = millis();

  // 1) Chegou mensagem do motor?
  if (chegouPacote) {
    PacoteMotor p;
    int rssi;
    portENTER_CRITICAL(&travaPacote);
    p = pacoteNovo;
    rssi = rssiNovo;
    chegouPacote = false;
    portEXIT_CRITICAL(&travaPacote);
    processarPacote(p, rssi, agora);
    precisaRedesenhar = true;
  }

  // 2) Zona de perigo e botão BOOT
  atualizarZona(agora);
  atualizarBotao(agora);

  // 3) Decide se bipa (normal / alarme / sem sinal...)
  EstadoPulseira novo = calcularEstado(agora);
  if (novo != estadoAtual) {
    EstadoPulseira antigo = estadoAtual;
    estadoAtual = novo;
    aoMudarEstado(antigo, novo);
    precisaRedesenhar = true;
  }

  atualizarBuzzer(agora);
  atualizarLed(agora);
  atualizarLedsSituacao(agora);

  // 4) Redesenha quando muda algo e a cada 0,5 s (pisca, contador e distância)
  uint32_t fase = agora / 500;
  if (precisaRedesenhar || fase != ultimaFase) {
    precisaRedesenhar = false;
    ultimaFase = fase;
    desenharTela(agora);
  }
  delay(2);
}
