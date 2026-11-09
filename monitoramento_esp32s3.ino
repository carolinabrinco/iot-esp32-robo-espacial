/*
 * ESP32-S3 de MONITORAMENTO
 * - Recebe pacotes via ESP-NOW do ESP32 do chao de fabrica
 * - Exibe as leituras numa matriz MAX7219 (MD_Parola), uma tela a cada 2 s
 * - LED verde = recepcao recente | LED vermelho = timeout (>5 s) ou erro
 * - Envia as leituras por HTTP (JSON) para a API, em uma task separada
 *
 * Ligacoes (ESP32-S3):
 *   MAX7219: VCC->5V, GND->GND (comum), DIN->GPIO11, CLK->GPIO12, CS->GPIO10
 *   LED verde   -> GPIO4 (resistor 220R) -> GND
 *   LED vermelho-> GPIO5 (resistor 220R) -> GND
 *
 * Arduino IDE: placa "ESP32S3 Dev Module"; se o Serial nao aparecer,
 * ative "USB CDC On Boot: Enabled".
 * Bibliotecas: MD_Parola e MD_MAX72XX (Gerenciador de Bibliotecas).
 */

#include <WiFi.h>
#include <esp_now.h>
#include <HTTPClient.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <math.h>
#include <time.h>

// =====================  CONFIGURACAO  =====================
const char* WIFI_SSID  = "ssid";
const char* WIFI_SENHA = "senha";
const char* URL_API    = "http://192.168.1.100:5000/leituras";  // IP LOCAL DO PC

// MAC do ESP32 transmissor (chao de fabrica). Tudo zero = aceita qualquer MAC (so para teste).
// Descubra o MAC do transmissor no Serial dele: WiFi.macAddress()
const uint8_t MAC_AUTORIZADO[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Matriz de LED. Se o texto aparecer espelhado/invertido/embaralhado, troque o tipo:
// FC16_HW (modulo azul comum), GENERIC_HW, PAROLA_HW, ICSTATION_HW
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES   1
#define DATA_PIN      11
#define CLK_PIN       12
#define CS_PIN        10

#define LED_VERDE     4
#define LED_VERMELHO  5

// 1 = fica so no teste de hardware ("HELLO OK" em loop, LEDs piscando). Use para validar a ligacao.
#define MODO_TESTE_HARDWARE 0

const unsigned long TIMEOUT_RX_MS  = 5000;  // sem dados por mais que isso = falha
const unsigned long TEMPO_TELA_MS  = 2000;  // cada tela fica 2 s
const uint8_t       VEL_SCROLL_MS  = 25;    // menor = mais rapido
const uint8_t       BRILHO         = 5;     // 0 a 15
// ==========================================================

// Matriz via SPI por software (funciona em qualquer pino do ESP32-S3).
MD_Parola matriz(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

// ESTRUTURA IDENTICA AO TRANSMISSOR ORIGINAL (NAO ALTERAR CAMPOS/ORDEM).
// Atencao: sao DOIS underscores de cada lado em __attribute__.
struct __attribute__((packed)) StructDados {
  int   nivel;
  float temp;
  float umi;
  int   luz;
  bool  presenca;
};

// Buffer circular em RAM para leituras pendentes de envio HTTP (perdido se faltar energia).
const int TAM_BUFFER = 60;
StructDados fila[TAM_BUFFER];
int inicio = 0, quantidade = 0;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Estado compartilhado entre callback ESP-NOW e loop (protegido por mux)
StructDados ultima = {};
bool temUltima = false;
unsigned long ultimaRxMs = 0;       // marcador de ultima recepcao (lastRxMillis)
unsigned long rxContador = 0;
unsigned long descartadas = 0;
unsigned long rejeitadas = 0;       // MAC nao autorizado / pacote invalido

// Estado do display
const char* NOMES_TELA[5] = {"NVL", "TMP", "UMD", "LUX", "PRS"};
int telaAtual = 0;
unsigned long ultimaTela = 0;
unsigned long ultimoWifi = 0;
unsigned long rxVisto = 0;
bool estadoOk = false;
bool estadoInicializado = false;
bool displayOk = false;
bool espNowOk = false;
char mensagem[32];

// ---------------------------------------------------------
// Regra de pontuacao sugerida no edital; nao e modelo de ML treinado.
float probabilidade(const StructDados &d) {
  float p = 0;
  if (d.temp >= 15 && d.temp <= 30) p += 25;
  if (d.umi >= 40 && d.umi <= 70) p += 25;
  if (d.luz > 500) p += 20;  // Ajustar limiar apos calibracao do LDR.
  if (d.presenca) p += 30;
  return p;
}

String agoraISO() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return String("sem-hora");
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
  return String(buf);
}

bool macAutorizado(const uint8_t *mac) {
  static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
  if (memcmp(MAC_AUTORIZADO, zero, 6) == 0) return true;  // sem filtro configurado
  return memcmp(mac, MAC_AUTORIZADO, 6) == 0;
}

// ---------------------------------------------------------
// Tratamento comum do pacote ESP-NOW (executa no contexto do Wi-Fi: nada de HTTP/Serial pesado aqui)
void tratarPacote(const uint8_t *mac, const uint8_t *dados, int len) {
  if (len != (int)sizeof(StructDados) || !macAutorizado(mac)) {
    rejeitadas++;
    return;
  }
  StructDados d;
  memcpy(&d, dados, sizeof(d));
  if (!isfinite(d.temp) || !isfinite(d.umi) || d.nivel < 0 || d.nivel > 100 || d.luz < 0) {
    rejeitadas++;
    return;
  }
  portENTER_CRITICAL(&mux);
  ultima = d;
  temUltima = true;
  ultimaRxMs = millis();
  rxContador++;
  if (quantidade < TAM_BUFFER) {
    fila[(inicio + quantidade) % TAM_BUFFER] = d;
    quantidade++;
  } else {
    descartadas++;  // Preserva leituras antigas para reenvio; registra perda.
  }
  portEXIT_CRITICAL(&mux);
}

// Assinatura do callback muda entre o core ESP32 2.x e 3.x
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void aoReceberDados(const esp_now_recv_info_t *info, const uint8_t *dados, int len) {
  tratarPacote(info->src_addr, dados, len);
}
#else
void aoReceberDados(const uint8_t *mac, const uint8_t *dados, int len) {
  tratarPacote(mac, dados, len);
}
#endif

// ---------------------------------------------------------
// Matriz de LED
void mostrar(const char *txt) {
  strlcpy(mensagem, txt, sizeof(mensagem));
  matriz.displayClear();
  matriz.displayText(mensagem, PA_LEFT, VEL_SCROLL_MS, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  matriz.displayReset();
}

void montarTela(const StructDados &d, int tela) {
  char txt[32];
  switch (tela) {
    case 0:  snprintf(txt, sizeof(txt), "NVL %d%%", d.nivel); break;
    case 1:  snprintf(txt, sizeof(txt), "TMP %.0fC", d.temp); break;
    case 2:  snprintf(txt, sizeof(txt), "UMD %.0f%%", d.umi); break;
    case 3:  snprintf(txt, sizeof(txt), "LUX %d", d.luz); break;
    default: snprintf(txt, sizeof(txt), "PRS %s", d.presenca ? "ON" : "OFF"); break;
  }
  mostrar(txt);
  Serial.printf("Tela -> %s (%s)\n", NOMES_TELA[tela], txt);
}

void definirLeds(bool ok) {
  digitalWrite(LED_VERDE, ok ? HIGH : LOW);
  digitalWrite(LED_VERMELHO, ok ? LOW : HIGH);
}

// ---------------------------------------------------------
// Envio HTTP
bool enviarHTTP(const StructDados &d) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient cliente;
  HTTPClient http;
  http.setTimeout(1200);
  if (!http.begin(cliente, URL_API)) return false;
  http.addHeader("Content-Type", "application/json");
  String json = "{\"temperatura_c\":" + String(d.temp, 2) +
                ",\"umidade_pct\":" + String(d.umi, 2) +
                ",\"luminosidade\":" + String(d.luz) +
                ",\"presenca\":" + String(d.presenca ? 1 : 0) +
                ",\"probabilidade_vida\":" + String(probabilidade(d), 1) +
                ",\"nivel\":" + String(d.nivel) + "}";
  // Timestamp UTC gerado no servidor quando omitido.
  int codigo = http.POST(json);
  http.end();
  Serial.printf("POST /leituras: HTTP %d\n", codigo);
  return codigo == 201;
}

// Task separada: o HTTP bloqueia por ate ~1,2 s e nao pode travar a animacao da matriz.
void tarefaHTTP(void *param) {
  for (;;) {
    StructDados d;
    bool disponivel = false;
    portENTER_CRITICAL(&mux);
    if (quantidade > 0) {
      d = fila[inicio];
      disponivel = true;
    }
    portEXIT_CRITICAL(&mux);

    if (!disponivel) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (enviarHTTP(d)) {
      portENTER_CRITICAL(&mux);
      inicio = (inicio + 1) % TAM_BUFFER;
      quantidade--;
      portEXIT_CRITICAL(&mux);
    } else {
      Serial.println("Falha: leitura mantida no buffer para nova tentativa");
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
  }
}

// ---------------------------------------------------------
void testeHardware() {
  Serial.println("TESTE DE HARDWARE: matriz + LEDs");
  digitalWrite(LED_VERDE, HIGH);
  digitalWrite(LED_VERMELHO, HIGH);
  mostrar("HELLO OK");
  while (!matriz.displayAnimate()) { delay(1); }
  definirLeds(false);
  delay(200);
  digitalWrite(LED_VERDE, LOW);
  digitalWrite(LED_VERMELHO, LOW);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_VERMELHO, OUTPUT);
  digitalWrite(LED_VERDE, LOW);
  digitalWrite(LED_VERMELHO, LOW);

  displayOk = matriz.begin();
  if (!displayOk) {
    Serial.println("ERRO: falha ao inicializar a matriz de LED");
    digitalWrite(LED_VERMELHO, HIGH);
  } else {
    matriz.setIntensity(BRILHO);
    matriz.displayClear();
    testeHardware();
  }

#if MODO_TESTE_HARDWARE
  for (;;) {
    testeHardware();
    delay(500);
  }
#endif

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // melhora a recepcao ESP-NOW
  WiFi.begin(WIFI_SSID, WIFI_SENHA);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
    delay(100);
  }
  configTime(0, 0, "pool.ntp.org");  // hora UTC para os logs
  // O canal do radio e o do roteador. O transmissor ESP-NOW deve usar o MESMO canal.
  Serial.printf("WiFi: %s, canal: %d, MAC: %s\n",
                WiFi.status() == WL_CONNECTED ? "conectado" : "offline",
                WiFi.channel(), WiFi.macAddress().c_str());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ERRO AO INICIALIZAR ESP-NOW");
    espNowOk = false;
  } else {
    esp_now_register_recv_cb(aoReceberDados);
    espNowOk = true;
    Serial.println("ESP-NOW PRONTO");
    xTaskCreatePinnedToCore(tarefaHTTP, "http", 8192, NULL, 1, NULL, 0);
  }

  ultimaRxMs = millis();
  ultimaTela = millis();
}

void loop() {
  // 1) Animacao da matriz (chamar sempre, sem bloquear)
  bool animacaoFim = displayOk ? matriz.displayAnimate() : false;

  // 2) Reconexao Wi-Fi periodica (nao bloqueia)
  if (WiFi.status() != WL_CONNECTED && millis() - ultimoWifi >= 10000) {
    ultimoWifi = millis();
    WiFi.reconnect();
  }

  // 3) Copia consistente do estado compartilhado
  StructDados d;
  bool tem;
  unsigned long ultRx, contador;
  portENTER_CRITICAL(&mux);
  d = ultima;
  tem = temUltima;
  ultRx = ultimaRxMs;
  contador = rxContador;
  portEXIT_CRITICAL(&mux);

  unsigned long agora = millis();
  bool recente = espNowOk && tem && (agora - ultRx <= TIMEOUT_RX_MS);

  // 4) Log de cada recepcao
  if (contador != rxVisto) {
    rxVisto = contador;
    Serial.printf("RX: nivel=%d%% temp=%.1fC umd=%.1f%% lux=%d prs=%d vida=%.0f%% ts=%s\n",
                  d.nivel, d.temp, d.umi, d.luz, d.presenca ? 1 : 0,
                  probabilidade(d), agoraISO().c_str());
  }

  // 5) Mudanca de estado: LEDs + tela de alerta / retomada do ciclo
  if (!estadoInicializado || recente != estadoOk) {
    estadoInicializado = true;
    estadoOk = recente;
    definirLeds(recente);
    if (recente) {
      Serial.println("LED VERDE ON - dados recebidos");
      telaAtual = 0;
      ultimaTela = agora;
      if (displayOk) montarTela(d, telaAtual);
    } else {
      Serial.println("LED VERMELHO ON - timeout de comunicacao");
      if (displayOk) mostrar("SEM DADOS");
    }
  }
  if (!displayOk) digitalWrite(LED_VERMELHO, HIGH);  // erro de exibicao = vermelho

  // 6) Ciclo de telas a cada 2 s
  if (displayOk) {
    if (recente) {
      if (agora - ultimaTela >= TEMPO_TELA_MS) {
        ultimaTela = agora;
        telaAtual = (telaAtual + 1) % 5;
        montarTela(d, telaAtual);
      } else if (animacaoFim) {
        matriz.displayReset();  // repete o scroll ate completar os 2 s
      }
    } else if (animacaoFim) {
      matriz.displayReset();    // repete "SEM DADOS"
    }
  }

  // 7) Resumo periodico no Serial
  static unsigned long ultimoResumo = 0;
  if (agora - ultimoResumo >= 5000) {
    ultimoResumo = agora;
    int pend;
    unsigned long desc, rej;
    portENTER_CRITICAL(&mux);
    pend = quantidade; desc = descartadas; rej = rejeitadas;
    portEXIT_CRITICAL(&mux);
    Serial.printf("Pendentes: %d | Descartadas: %lu | Rejeitadas: %lu\n", pend, desc, rej);
  }
}