/*
  ============================================================
   ESP3 - ZONA DE PERIGO (farol)  -  Projeto SentryBand
   Placa: ESP32-S3 mini (serve qualquer ESP32)
   Autor: Noberto
  ============================================================

  O que este código faz:
    - Fica parado no centro da zona de perigo (perto da máquina) e manda
      um "sinal de farol" pelo ESP-NOW 10 vezes por segundo.
    - A pulseira (ESP1) mede a força desse sinal: quanto mais forte, mais
      perto. Se ela chegar mais perto que a distância escolhida, bipa.
    - A distância e a calibração ficam na pulseira (botão BOOT dela).

  Ligações: nenhuma. É só alimentar pela USB (carregador ou power bank).
  Não mude a POTENCIA_TX depois de calibrar a pulseira.

  Placa: "ESP32S3 Dev Module" (pacote esp32 da Espressif, versão 3.x)
    Ferramentas > "USB CDC On Boot" > Enabled  (para ver as mensagens pela USB)
*/

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#if !defined(ESP_ARDUINO_VERSION_MAJOR) || ESP_ARDUINO_VERSION_MAJOR < 3
#error "Atualize o pacote de placas esp32 para a versao 3.x (Ferramentas > Placa > Gerenciador de Placas)"
#endif

// ======================= CONFIGURAÇÕES =======================
#define ID_ZONA             1                  // número desta zona (1 a 255)
#define CANAL_ESPNOW        1                  // tem que ser IGUAL na pulseira e no motor
#define INTERVALO_FAROL_MS  100                // 10 sinais por segundo
#define POTENCIA_TX         WIFI_POWER_8_5dBm  // força fixa do sinal: não mude depois de calibrar

// ============ PACOTE DO FAROL (tem que ser IGUAL na pulseira) ============
#define ZONA_MAGICO 0x4E425A31UL   // "NBZ1": marca que o sinal é da zona
#define ZONA_VERSAO 1

typedef struct __attribute__((packed)) {
  uint32_t magico;      // sempre ZONA_MAGICO
  uint8_t  versao;      // ZONA_VERSAO
  uint8_t  idZona;      // ID_ZONA
  uint32_t sequencia;   // 1, 2, 3...
} PacoteZona;

// ======================= VARIÁVEIS =======================
const uint8_t ENDERECO_BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint32_t sequencia = 0;
uint32_t ultimoFarol = 0;
uint32_t falhas = 0;

// ======================= RÁDIO (ESP-NOW) =======================

// Liga o rádio no modo ESP-NOW, no canal fixo e com a força fixa, enviando para "todos"
void iniciarEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setTxPower(POTENCIA_TX);

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

// ======================= SETUP E LOOP =======================

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("===== ESP3 - ZONA DE PERIGO (farol) =====");
  iniciarEspNow();
  Serial.print("MAC deste ESP32: ");
  Serial.println(WiFi.macAddress());
  Serial.printf("Zona %d no canal %d: %d sinais por segundo\n", ID_ZONA, CANAL_ESPNOW, 1000 / INTERVALO_FAROL_MS);
}

void loop() {
  uint32_t agora = millis();
  if (agora - ultimoFarol < INTERVALO_FAROL_MS) return;
  ultimoFarol = agora;

  PacoteZona pacote = {};
  pacote.magico    = ZONA_MAGICO;
  pacote.versao    = ZONA_VERSAO;
  pacote.idZona    = ID_ZONA;
  pacote.sequencia = ++sequencia;
  if (esp_now_send(ENDERECO_BROADCAST, (const uint8_t *)&pacote, sizeof(pacote)) != ESP_OK) {
    falhas++;
  }

  // a cada 5 s, um resumo no Monitor Serial
  if (sequencia % 50 == 0) {
    Serial.printf("Farol: %lu sinais enviados, %lu falhas\n", (unsigned long)sequencia, (unsigned long)falhas);
  }
}
