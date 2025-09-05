#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <MFRC522.h>

// === Cartão SD ===
#define SD_CS 33
// SPIClass para SD no HSPI
SPIClass spiSD(HSPI);

bool limiteCarregadoOffline = false; //controle do limite carregado offline
//para controlar quando o gps nao funciona e precisamos de um limite offline
//ela evita que o sistema tente carregar repetidamente o limite do cartão
StaticJsonDocument<256> motoristaAtual;
bool motoristaEncontrado = false;

// === GPS ===
#define RX_GPS 16
#define TX_GPS -1
HardwareSerial gpsSerial(2);  // UART2
TinyGPSPlus gps;

bool gpsAtivo = false;

// === LCD ===
LiquidCrystal_I2C lcd(0x27, 16, 2);
boolean lcdFlag = false; //flag para controlar a impressão no lcd
//ajuda a impedir impressão constante desnecessária

// === Wi-Fi e API ===
const char* ssid = "Duarte_Fotos";
const char* password = "05519558213";
const char* apiURL = "https://telemetria-fvv4.onrender.com/cercas";
const char* apiMotoristas = "https://telemetria-fvv4.onrender.com/motoristas/limpo";

// === Atualização periódica ===
// unsigned long ultimaAtualizacao = 0;
// const unsigned long intervaloAtualizacao = 5 * 60 * 1000; // 5 minutos
unsigned long ultimaVerificacaoCercas = 0;
unsigned long intervaloVerificacaoCercas = 15000; // 5 segundos

int vel_max;
int vel_max_chuva;

float vel = 0;
unsigned long ultimaImpressaoVel = 0;

#define potenciometro 34
#define vel_max_potenciometro 80

//============= paraa lógica de tolerancia de 10 segundos
bool alertaVelocidade = false;
unsigned long inicioAlerta = 0;
const unsigned long TOLERANCIA_ALERTA = 10000; // 10 segundos
const unsigned long INTERVALO_NORMAL = 15000; // 15 segundos padrão
const unsigned long INTERVALO_ALERTA = 3000;  // 3 segundos durante alerta
bool chuva = false; // sensor IR

// === RFID ===
#define RST_PIN 27
#define SS_PIN_RFID 15

MFRC522 mfrc522(SS_PIN_RFID, RST_PIN);
SPIClass spiRFID(VSPI);  // VSPI para RFID

boolean rfidLido = false;
String rfidValor;
String ultimoUID = "";
unsigned long viagemId = 0;

#define LED_PIN 2  // LED embutido do ESP32
#define BUZZER_PIN 17

//variaveis auxiliares para a logica de registrar viajem offline
//para que os dados sejam armazenados no cartão
File arquivoViagem;
String nomeArquivoViagem = "";
bool viagemAtiva = false;
// bool primeiroRegistro = true;

// === ID do Veículo ===
const int VEICULO_ID = 2;

// vairáveis para a lógica de registro e recuperação de viajens não encerradas
const String ARQUIVO_VIAGEM_ATUAL = "/viagem_atual.tmp";
bool viagemRecuperada = false;

//para as funções de escrita e leitura de registros
char jsonBuffer[256];

//prototipo das funções
void verificarMotoristaPorRFID();
void salvarUltimoLimite();
void encerrarViagem(float destinoLat, float destinoLng);

unsigned long ultimaLeituraRFID = 0;
const unsigned long DEBOUNCE_RFID = 1000;

// para a logica de atualização assincrona ------------------------------
TaskHandle_t taskAtualizacaoHandle = NULL;
bool atualizacaoEmAndamento = false;
unsigned long proximaAtualizacao = 0;
const unsigned long INTERVALO_ATUALIZACAO = 5 * 60 * 1000; // 5 minutos
//prototipos
void taskAtualizacao(void* parameter);
void iniciarAtualizacaoAssincrona();

// para a logica de envio a API -----------------------------------------
TaskHandle_t taskEnvioViagensHandle = NULL;
bool envioViagensAtivo = false;
// const unsigned long INTERVALO_ENVIO_VIAGENS = 30000; // 30 segundos
const char* apiRegistrarViagem = "https://telemetria-fvv4.onrender.com/viagens/registrar-viagem";

// Protótipos
void taskEnvioViagens(void* parameter);
void iniciarEnvioViagens();
bool enviarViagemParaAPI(const String& caminhoArquivo);


void processarCartao(String uid) {
  if (millis() - ultimaLeituraRFID < DEBOUNCE_RFID) {
    Serial.println("⏰ Debounce RFID: leitura ignorada (muito rápida)");
    return;
  }
  ultimaLeituraRFID = millis();

  // Guarda o UID atual para verificação
  String uidAnterior = rfidValor;
  rfidValor = uid;
  
  // Verifica se é um motorista autorizado
  verificarMotoristaPorRFID();
  
  if (!motoristaEncontrado) {
    Serial.println("❌ Cartão não autorizado");
    rfidValor = uidAnterior; // Restaura o UID anterior
    return;
  }

  if (!rfidLido) {
    // Início da viagem
    rfidLido = true;
    ultimoUID = uid;
    lcdFlag = true;
    Serial.println("🚗 Viagem iniciada");
  } else {
    // Se já há viagem em andamento
    if (uid == ultimoUID) {
      // Mesmo motorista - encerra viagem
      rfidLido = false;
      limiteCarregadoOffline = false;
      salvarUltimoLimite();
      motoristaEncontrado = false;
      ultimoUID = "";
      // viagemId = 0;
      lcd.clear();
      lcd.print("Viagem encerrada");
      Serial.println("🛑 Viagem encerrada");

      encerrarViagem();

      delay(3000);
      lcd.clear();
      lcd.print("Inicie a viagem");
    } else {
      // Motorista diferente - encerra viagem atual e inicia nova
      Serial.println("🔄 Motorista diferente - trocando viagem");
      
      // Primeiro encerra a viagem atual
      encerrarViagem();
      
      // Limpa variáveis da viagem anterior
      rfidLido = false;
      limiteCarregadoOffline = false;
      salvarUltimoLimite();
      ultimoUID = "";
      
      // Inicia nova viagem com o novo motorista
      rfidLido = true;
      ultimoUID = uid;
      lcdFlag = true;
      
      Serial.println("🚗 Nova viagem iniciada com outro motorista");
      
      // Feedback visual
      lcd.clear();
      lcd.print("Troca motorista");
      lcd.setCursor(0, 1);
      lcd.print(motoristaAtual["nome"].as<const char*>());
      
      delay(2000);
    }
  }
}

//------------------------------------------------------------------------

void taskRFID(void* parameter) {
  Serial.println("RFID pronto");
  bool cartaoPresente = false;

  unsigned long ultimaLeitura = 0;
  static unsigned long ultimaVerificacao = millis();
  const unsigned long intervaloAntiRepeticao = 1000; // 1 segundo entre leituras

  for (;;) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      String uid = "";
      for (byte i = 0; i < mfrc522.uid.size; i++) {
        if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
        uid += String(mfrc522.uid.uidByte[i], HEX);
      }
      uid.toUpperCase();

      // Prevenção contra leituras repetidas muito rápidas
      if (millis() - ultimaLeitura > intervaloAntiRepeticao) {
        ultimaLeitura = millis();

        if (!cartaoPresente) {
          // Apenas processa quando o cartão for inserido novamente
          digitalWrite(LED_PIN, HIGH);
          digitalWrite(BUZZER_PIN, HIGH);
          delay(50);
          digitalWrite(BUZZER_PIN, LOW);
          delay(35);
          digitalWrite(BUZZER_PIN, HIGH);
          delay(50);
          digitalWrite(BUZZER_PIN, LOW);

          Serial.print("🔍 UID lido: ");
          Serial.println(uid);

          rfidValor = uid;
          processarCartao(uid);
          cartaoPresente = true;
        }
      }

      mfrc522.PICC_HaltA();
    } else {
      if (cartaoPresente) {
        digitalWrite(LED_PIN, LOW);
        cartaoPresente = false; // Liberar para próxima leitura
        // Pequeno delay após remover o cartão para evitar leitura fantasma
        vTaskDelay(300 / portTICK_PERIOD_MS);
      }
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

float lerVelocidadePotenciometro() {
  int leitura = analogRead(potenciometro);
  // Converte leitura ADC (0-4095) para velocidade (0-80 km/h)
  return map(leitura, 0, 4095, 0, vel_max_potenciometro);
}

//------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(potenciometro, INPUT);

  limiteCarregadoOffline = false;

  pinMode(RST_PIN, OUTPUT);
  digitalWrite(RST_PIN, HIGH);
  delay(3000);

  gpsSerial.begin(9600, SERIAL_8N1, RX_GPS, TX_GPS);

  unsigned long inicioGPS = millis();
  bool gpsRespondendo = false;
  while (millis() - inicioGPS < 3000) { // 3 segundos de teste
      while (gpsSerial.available()) {
          char c = gpsSerial.read();
          Serial.write(c); // imprime cada caractere vindo do GPS
          gpsRespondendo = true;
      }
  }

  if (gpsRespondendo) {
      Serial.println("\n✅ GPS respondendo!");
  } else {
      Serial.println("\n⚠️ GPS não respondeu, verifique conexões.");
  }

  Wire.begin(4, 5);
  lcd.begin(16, 2);
  lcd.backlight();
  lcd.clear();
  lcd.print("Iniciando");
  lcd.setCursor(0, 1);
  lcd.print("Aguarde...");

  // Iniciar SPI do rfid
  digitalWrite(RST_PIN, LOW);
  delay(200);
  digitalWrite(RST_PIN, HIGH);
  delay(200);
  // delay(3000);

  SPI.begin(14, 12, 13, SS_PIN_RFID);
  // spiRFID.begin(14, 12, 13, SS_PIN_RFID);
  mfrc522.PCD_Init();
  delay(1000);
  Serial.print("RFID Version: ");
  byte v = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  Serial.println(v, HEX);

  // Serial.println("Conectando ao Wi-Fi...");
  WiFi.begin(ssid, password);
  unsigned long inicioWifi = millis();

  // Inicia SD no HSPI (pinos 25,26,21,33)
  spiSD.begin(25, 26, 21, SD_CS);
  if (!SD.begin(SD_CS, spiSD)) {
    Serial.println("❌ Falha ao iniciar cartão SD");
    lcd.begin(16, 2);
    lcd.backlight();
    lcd.clear();
    lcd.print("Falha no SD!");
    while(true) delay(1000); // trava se falhar no SD
  }
  Serial.println("✅ Cartão SD iniciado");

  if (!SD.exists("/viagens")) {
    SD.mkdir("/viagens");
    Serial.println("📁 Diretório /viagens criado");
  }

  if (!SD.exists("/pendente")) {
    SD.mkdir("/pendente");
    Serial.println("📁 Diretório /pendente criado");
  }

  // Verificar se há viagens pendentes para recuperar
  File pendenteDir = SD.open("/pendente");
  if (pendenteDir && pendenteDir.isDirectory()) {
      bool temArquivos = false;
      while (true) {
          File entry = pendenteDir.openNextFile();
          if (!entry) break;
          if (!entry.isDirectory() && String(entry.name()).endsWith(".json")) {
              temArquivos = true;
              entry.close();
              break;
          }
          entry.close();
      }
      pendenteDir.close();
      
      if (temArquivos) {
          Serial.println("⚠️ Viagens pendentes encontradas. Recuperando...");
          recuperarViagemInterrompida();
      }
  }

  //para o rfid
  xTaskCreatePinnedToCore(
    taskRFID,     // Função da tarefa
    "RFID Reader",// Nome da tarefa
    4096,         // Tamanho da stack
    NULL,         // Parâmetro
    1,            // Prioridade
    NULL,         // Handle da tarefa
    0             // Core 0 (o loop() roda no Core 1)
  );

  //para a atualização de cercas
  xTaskCreatePinnedToCore(
    taskAtualizacao,
    "Atualizacao",
    8192,
    NULL,
    1,
    &taskAtualizacaoHandle,
    0
  );
  //ja atualiza os dados no inicio do sistema
  proximaAtualizacao = millis() + 10000;

  xTaskCreatePinnedToCore(
      taskEnvioViagens,
      "EnvioViagens",
      4096,
      NULL,
      1,
      &taskEnvioViagensHandle,
      0
  );

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Iniciando gps...");
}

//------------------------------------------------------------------------

void loop() {

  vel = lerVelocidadePotenciometro();

  if (millis() > proximaAtualizacao) {
    iniciarAtualizacaoAssincrona();
  }

  if (rfidLido) {

    if (rfidLido && lcdFlag) {
      delay(50);
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Bem vindo(a)");
      lcd.setCursor(0, 1);
      lcd.print(motoristaAtual["nome"].as<const char*>());

      iniciarViagem();
      delay(3000);

      lcdFlag = false;
    }

    if (!gpsAtivo && !limiteCarregadoOffline) {
      if (carregarUltimoLimite()) {
        lcd.clear();
        lcd.setCursor(0, 1);
        lcd.print("Limite:");
        lcd.setCursor(8, 1);
        lcd.print(vel_max);
        lcd.print("km/h");

        Serial.println("✅ Limite carregado do SD na ausência do GPS.");
        limiteCarregadoOffline = true;
      } else {
        Serial.println("⚠️ Falha ao carregar limite do SD.");
      }
    }

    int limiteAtual = chuva ? vel_max_chuva : vel_max;

    if (vel > limiteAtual && !alertaVelocidade) {
      alertaVelocidade = true;
      inicioAlerta = millis();
      digitalWrite(BUZZER_PIN, !digitalRead(BUZZER_PIN));
      Serial.println("🚨 Velocidade acima do limite! Buzzer ativado.");
    }

    if (alertaVelocidade) {
      // Verifica se motorista reduziu dentro dos 10 segundos
      if (vel <= limiteAtual) {
        alertaVelocidade = false;
        digitalWrite(BUZZER_PIN, LOW);
        Serial.println("✅ Velocidade normalizada. Alerta cancelado.");
      }
      else if (millis() - inicioAlerta > TOLERANCIA_ALERTA) {
        // Motorista não reduziu, diminui intervalo de verificação
        intervaloVerificacaoCercas = INTERVALO_ALERTA;
        Serial.println("⚠️ Intervalo reduzido para 3s (alerta ativo)");
      }
    }
    else if (intervaloVerificacaoCercas != INTERVALO_NORMAL) {
      intervaloVerificacaoCercas = INTERVALO_NORMAL;
      Serial.println("↩️ Intervalo restaurado para 15s");
    }

    while (gpsSerial.available()) {
      gps.encode(gpsSerial.read());

      if (gps.location.isUpdated()) {
        gpsAtivo = true;

        float lat = gps.location.lat();
        float lng = gps.location.lng();
        // float vel = gps.speed.kmph(); // velocidade do obd2

        if (millis() - ultimaVerificacaoCercas > intervaloVerificacaoCercas) {

          if (!alertaVelocidade || (alertaVelocidade && millis() - inicioAlerta > TOLERANCIA_ALERTA)) {
            Serial.print("Lat: "); Serial.println(lat, 6);
            Serial.print("Lng: "); Serial.println(lng, 6);
            ultimaVerificacaoCercas = millis();

            verificarCercas(lat, lng);
            registrarPosicao(lat, lng, vel, chuva);
            iniciarEnvioViagens();
          }
        }
      }
    }

    if (millis() - ultimaImpressaoVel > 500) {
      lcd.setCursor(0, 0); 
      lcd.print("                ");
      lcd.setCursor(0, 0); 
      lcd.print(vel);
      lcd.setCursor(4, 0); 
      lcd.print("km/h");
      ultimaImpressaoVel = millis();
    }
    
  } else {

    if (!lcdFlag) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Inicie a Viagem");
      lcdFlag = true;
    }
  }

  //salvamento automatico que independe do gps
  //o salvamento antigo só era feito ao registrar uma nova posição
  //agora é feito independente do sinal do gps
  static unsigned long ultimoSave = 0;
  if (viagemAtiva && millis() - ultimoSave > 30000) {
    arquivoViagem.flush();
    ultimoSave = millis();
    Serial.println("💾 Dados da viagem salvos (auto-salvamento)");
  }
}

// para o envio das viagens para a API
bool enviarViagemParaAPI(const String& caminhoArquivo) {
    File file = SD.open(caminhoArquivo, FILE_READ);
    if (!file) {
        Serial.println("❌ Erro ao abrir arquivo para envio");
        return false;
    }
    
    // Lê todo o conteúdo do arquivo
    String jsonData;
    while (file.available()) {
        jsonData += (char)file.read();
    }
    file.close();
    
    // 🔥 IMPORTANTE: Completa o JSON antes de enviar (se estiver incompleto)
    if (!jsonData.endsWith("]}")) {
        jsonData += "]}"; // Fecha o array de registros e o objeto principal
        Serial.println("🔧 JSON completado com fechamento");
    }
    
    Serial.print("📊 Tamanho do JSON: ");
    Serial.print(jsonData.length());
    Serial.println(" bytes");
    
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    
    if (https.begin(client, apiRegistrarViagem)) {
        https.addHeader("Content-Type", "application/json");
        https.addHeader("User-Agent", "ESP32-Telemetria");
        
        Serial.println("🔄 Enviando para API...");
        int httpCode = https.POST(jsonData);
        
        if (httpCode == 200 || httpCode == 201) {
            Serial.println("✅ Viagem enviada com sucesso!");
            https.end();
            
            // ✅ APENAS LOG - NÃO REMOVE O ARQUIVO!
            // O sistema principal já cuida disso através da recuperarViagemInterrompida()
            Serial.println("📋 Arquivo mantido em /pendente para processamento normal");
            return true;
        } else {
            Serial.print("❌ Erro HTTP no envio: ");
            Serial.println(httpCode);
            Serial.print("Resposta: ");
            Serial.println(https.getString());
            https.end();
            return false;
        }
    } else {
        Serial.println("❌ Erro ao iniciar conexão HTTP");
        return false;
    }
}

void taskEnvioViagens(void* parameter) {
    Serial.println("📤 Task de envio de viagens iniciada");
    
    for (;;) {
        if (envioViagensAtivo && WiFi.status() == WL_CONNECTED) {
            Serial.println("🔍 Verificando viagens pendentes para envio...");
            
            File pendenteDir = SD.open("/pendente");
            if (pendenteDir && pendenteDir.isDirectory()) {
                bool enviouAlguma = false;
                
                while (true) {
                    File entry = pendenteDir.openNextFile();
                    if (!entry) break;
                    
                    String nomeArquivo = String(entry.name());
                    entry.close();
                    
                    // Ignora diretórios e arquivos que não são .json
                    if (nomeArquivo.equals(".") || nomeArquivo.equals("..") || 
                        !nomeArquivo.endsWith(".json")) {
                        continue;
                    }
                    
                    String caminhoCompleto = "/pendente/" + nomeArquivo;
                    Serial.print("📨 Tentando enviar: ");
                    Serial.println(nomeArquivo);
                    
                    if (enviarViagemParaAPI(caminhoCompleto)) {
                        enviouAlguma = true;
                        Serial.println("✅ Envio bem-sucedido");
                        // ✅ NÃO REMOVE - o arquivo fica em /pendente para o processamento normal
                    } else {
                        Serial.println("❌ Falha no envio, será tentado novamente depois");
                    }
                    
                    // Pequena pausa entre envios
                    vTaskDelay(1000 / portTICK_PERIOD_MS);
                }
                
                pendenteDir.close();
                
                if (!enviouAlguma) {
                    Serial.println("📭 Nenhuma viagem pendente para enviar");
                }
            } else {
                Serial.println("❌ Erro ao abrir pasta /pendente");
            }
            
            envioViagensAtivo = false;
        }
        
        vTaskDelay(5000 / portTICK_PERIOD_MS); // Verifica a cada 5 segundos
    }
}

void iniciarEnvioViagens() {
    if (WiFi.status() == WL_CONNECTED && !envioViagensAtivo) {
        envioViagensAtivo = true;
        Serial.println("📤 Solicitação de envio de viagens ativada");
    }
}

// lógicas para motoristas e cercas ------------------------------------------------------------------------
// === Função para atualização assíncrona ===
void taskAtualizacao(void* parameter) {
  Serial.println("🔄 Tarefa de atualização iniciada");
  
  for (;;) {
    if (atualizacaoEmAndamento) {
      Serial.println("🔄 Iniciando atualização assíncrona de cercas e motoristas...");
      
      // Fazer ambas as requisições primeiro para aproveitar a janela de conexão
      WiFiClientSecure clientCercas;
      WiFiClientSecure clientMotoristas;
      HTTPClient httpsCercas;
      HTTPClient httpsMotoristas;
      
      bool cercasSucesso = false;
      bool motoristasSucesso = false;
      
      // REQUISIÇÃO DE CERCAS
      clientCercas.setInsecure();
      if (httpsCercas.begin(clientCercas, apiURL)) {
        httpsCercas.addHeader("User-Agent", "ESP8266");
        httpsCercas.addHeader("Accept", "application/json");
        httpsCercas.addHeader("Accept-Encoding", "identity");
        httpsCercas.setTimeout(10000);

        int httpCode = httpsCercas.GET();
        if (httpCode == 200) {
          Serial.println("✅ Resposta OK das cercas. Preparando para salvar...");
          cercasSucesso = true;
        } else {
          Serial.print("⚠️ Falha na requisição de cercas. Código HTTP: ");
          Serial.println(httpCode);
          httpsCercas.end();
        }
      }

      // Pequeno delay entre as requisições
      vTaskDelay(300 / portTICK_PERIOD_MS);

      // REQUISIÇÃO DE MOTORISTAS
      clientMotoristas.setInsecure();
      if (httpsMotoristas.begin(clientMotoristas, apiMotoristas)) {
        httpsMotoristas.addHeader("User-Agent", "ESP8266");
        httpsMotoristas.addHeader("Accept", "application/json");
        httpsMotoristas.addHeader("Accept-Encoding", "identity");
        httpsMotoristas.setTimeout(10000);

        int httpCode = httpsMotoristas.GET();
        if (httpCode == 200) {
          Serial.println("✅ Resposta OK dos motoristas. Preparando para salvar...");
          motoristasSucesso = true;
        } else {
          Serial.printf("Erro HTTP motoristas: %d\n", httpCode);
          httpsMotoristas.end();
        }
      }

      // AGORA PROCESSAR OS DADOS (se as requisições foram bem sucedidas)
      
      // PROCESSAR CERCAS
      if (cercasSucesso) {
        Serial.println("💾 Salvando dados das cercas...");
        File temp = SD.open("/temp_cercas.json", FILE_WRITE);
        if (temp) {
          WiFiClient& stream = httpsCercas.getStream();

          unsigned long inicio = millis();
          const unsigned long tempoLimite = 10000;
          bool iniciouJson = false;
          bool transmissaoAtiva = true;

          while (transmissaoAtiva && (millis() - inicio) < tempoLimite) {
            if (stream.available()) {
              char c = stream.read();

              if (!iniciouJson) {
                if (c == '[' || c == '{') {
                  iniciouJson = true;
                  temp.write(c);
                }
              } else {
                temp.write(c);
              }

              inicio = millis();
            } else {
              vTaskDelay(100 / portTICK_PERIOD_MS);
              
              if (!stream.connected() && stream.available() == 0) {
                transmissaoAtiva = false;
                Serial.println("📦 Transmissão de cercas concluída");
              }
            }
            
            vTaskDelay(10 / portTICK_PERIOD_MS);
          }

          temp.flush();
          temp.close();
          httpsCercas.end();
          Serial.println("✅ Dados das cercas salvos temporariamente");

          if (validarEstruturaJSON("/temp_cercas.json")) {
            Serial.println("✅ JSON de cercas válido! Substituindo arquivo oficial...");
            if (SD.exists("/cercas.json")) SD.remove("/cercas.json");
            SD.rename("/temp_cercas.json", "/cercas.json");
            Serial.println("📝 Cercas atualizadas com sucesso.");
          } else {
            Serial.println("⚠️ JSON de cercas inválido. Mantendo arquivo antigo.");
          }
        } else {
          Serial.println("❌ Erro ao abrir arquivo temporário para cercas.");
          httpsCercas.end();
        }
      }

      // PROCESSAR MOTORISTAS
      if (motoristasSucesso) {
        Serial.println("💾 Salvando dados dos motoristas...");
        File temp = SD.open("/temp_motoristas.json", FILE_WRITE);
        if (temp) {
          WiFiClient& stream = httpsMotoristas.getStream();
          unsigned long inicio = millis();
          const unsigned long tempoLimite = 10000;
          bool iniciouJson = false;
          bool transmissaoAtiva = true;

          while (transmissaoAtiva && (millis() - inicio) < tempoLimite) {
            if (stream.available()) {
              char c = stream.read();
              if (!iniciouJson) {
                if (c == '[' || c == '{') {
                  iniciouJson = true;
                  temp.write(c);
                }
              } else {
                temp.write(c);
              }
              inicio = millis();
            } else {
              vTaskDelay(100 / portTICK_PERIOD_MS);
              
              if (!stream.connected() && stream.available() == 0) {
                transmissaoAtiva = false;
                Serial.println("📦 Transmissão de motoristas concluída");
              }
            }
            
            vTaskDelay(10 / portTICK_PERIOD_MS);
          }

          temp.flush();
          temp.close();
          httpsMotoristas.end();
          Serial.println("✅ Dados dos motoristas salvos temporariamente");

          if (validarEstruturaJSON("/temp_motoristas.json")) {
            Serial.println("🧾 JSON de motoristas válido.");
            if (SD.exists("/motoristas.json")) SD.remove("/motoristas.json");
            SD.rename("/temp_motoristas.json", "/motoristas.json");
            Serial.println("📦 Motoristas atualizados com sucesso.");
          } else {
            Serial.println("❌ JSON de motoristas inválido.");
          }
        } else {
          Serial.println("❌ Erro ao abrir temp_motoristas.json.");
          httpsMotoristas.end();
        }
      }

      Serial.println("✅ Atualização assíncrona concluída");
      atualizacaoEmAndamento = false;
      proximaAtualizacao = millis() + INTERVALO_ATUALIZACAO;
    }
    
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void iniciarAtualizacaoAssincrona() {
  if (WiFi.status() == WL_CONNECTED && !atualizacaoEmAndamento) {
    atualizacaoEmAndamento = true;
    Serial.println("🔄 Solicitando atualização assíncrona...");
  }
}

// void atualizarCercas() {
//   Serial.println("🔄 Atualizando lista de cercas...");
//   WiFiClientSecure client;
//   client.setInsecure();
//   HTTPClient https;

//   if (https.begin(client, apiURL)) {
//     https.addHeader("User-Agent", "ESP8266");
//     https.addHeader("Accept", "application/json");
//     https.addHeader("Accept-Encoding", "identity");
//     https.setTimeout(10000); // 10 segundos de espera de resposta da API


//     int httpCode = https.GET();
//     if (httpCode == 200) {
//       Serial.println("Resposta OK. Salvando no arquivo temporário...");

//       // Salva em arquivo temporário
//       File temp = SD.open("/temp_cercas.json", FILE_WRITE);
//       if (temp) {
//         WiFiClient& stream = https.getStream();

//         unsigned long inicio = millis();
//         const unsigned long tempoLimite = 10000; // 10 segundos
//         bool iniciouJson = false;

//         while ((millis() - inicio) < tempoLimite) {
//           if (stream.available()) {
//             char c = stream.read();

//             if (!iniciouJson) {
//               if (c == '[' || c == '{') {
//                 iniciouJson = true;
//                 temp.write(c);
//               }
//             } else {
//               temp.write(c);
//             }

//             inicio = millis(); // Reinicia tempo sempre que lê algo
//           }
//         }

//         temp.flush();
//         temp.close();
//         Serial.println("✅ Arquivo temporário salvo com sucesso!");

//         if (validarEstruturaJSON("/temp_cercas.json")) {
//           Serial.println("✅ JSON parece válido! Substituindo arquivo oficial...");

//           if (SD.exists("/cercas.json")) {
//             SD.remove("/cercas.json");
//           }

//           SD.rename("/temp_cercas.json", "/cercas.json");
//           Serial.println("📝 Substituição concluída.");
//         } else {
//           Serial.println("⚠️ JSON inválido (estrutura incompleta). Mantendo arquivo antigo.");
//         }

//       } else {
//         Serial.println("❌ Erro ao abrir arquivo temporário para escrita.");
//       }
//     } else {
//       Serial.print("⚠️ Falha na requisição. Código HTTP: ");
//       Serial.println(httpCode);
//     }
    
//     https.end();

//   } else {
//     Serial.println("❌ Erro ao iniciar conexão HTTPS.");
//   }
// }

// void atualizarMotoristas() {
//   Serial.println("🔄 Atualizando lista de motoristas...");
//   WiFiClientSecure client;
//   client.setInsecure();
//   HTTPClient https;

//   if (https.begin(client, apiMotoristas)) {
//     https.addHeader("User-Agent", "ESP8266");
//     https.addHeader("Accept", "application/json");
//     https.addHeader("Accept-Encoding", "identity");
//     https.setTimeout(10000);

//     int httpCode = https.GET();
//     if (httpCode == 200) {
//       Serial.println("🔄 Baixando motoristas...");

//       File temp = SD.open("/temp_motoristas.json", FILE_WRITE);
//       if (temp) {
//         WiFiClient& stream = https.getStream();
//         unsigned long inicio = millis();
//         const unsigned long tempoLimite = 10000;
//         bool iniciouJson = false;

//         while ((millis() - inicio) < tempoLimite) {
//           if (stream.available()) {
//             char c = stream.read();
//             if (!iniciouJson) {
//               if (c == '[' || c == '{') {
//                 iniciouJson = true;
//                 temp.write(c);
//               }
//             } else {
//               temp.write(c);
//             }
//             inicio = millis();
//           }
//         }

//         temp.flush();
//         temp.close();
//         Serial.println("✅ Motoristas temporários salvos.");

//         if (validarEstruturaJSON("/temp_motoristas.json")) {
//           Serial.println("🧾 JSON de motoristas válido.");

//           if (SD.exists("/motoristas.json")) {
//             SD.remove("/motoristas.json");
//           }
//           SD.rename("/temp_motoristas.json", "/motoristas.json");
//           Serial.println("📦 motoristas.json atualizado com sucesso.");
//         } else {
//           Serial.println("❌ JSON de motoristas inválido.");
//         }

//       } else {
//         Serial.println("❌ Erro ao abrir temp_motoristas.json.");
//       }
//     } else {
//       Serial.printf("Erro HTTP: %d\n", httpCode);
//     }

//     https.end();
//   } else {
//     Serial.println("❌ Erro ao iniciar conexão com motoristas.");
//   }
// }

void verificarMotoristaPorRFID() {
  File file = SD.open("/motoristas.json");
  if (!file) {
    Serial.println("❌ Falha ao abrir motoristas.json");
    motoristaEncontrado = false;
    return;
  }

  char c;
  do {
    c = file.read();
  } while (c != -1 && isspace(c));

  if (c != '[') {
    Serial.println("Formato inválido em motoristas.json");
    file.close();
    motoristaEncontrado = false;
    return;
  }

  StaticJsonDocument<256> doc;

  while (file.available()) {
    DeserializationError err = deserializeJson(doc, file);
    if (err) {
      Serial.print("Erro ao ler motorista: ");
      Serial.println(err.c_str());
      break;
    }

    JsonObject motorista = doc.as<JsonObject>();
    const char* rfid = motorista["cartao_rfid"];

    if (rfidValor.equalsIgnoreCase(rfid)) {
      motoristaAtual.clear();
      motoristaAtual.set(motorista);
      motoristaEncontrado = true;
      Serial.println("✅ Motorista encontrado:");
      Serial.print("  Nome: "); Serial.println(motorista["nome"].as<const char*>());
      break;
    }

    bool fim = false;
    while (file.available()) {
      char next = file.peek();
      if (next == ',') {
        file.read();
        break;
      } else if (isspace(next)) {
        file.read();
      } else if (next == ']') {
        file.read();
        fim = true;
        break;
      } else {
        break;
      }
    }

    if (fim) break;
    doc.clear();
  }

  file.close();

  if (!motoristaEncontrado) {
    Serial.println("❌ Nenhum motorista com esse RFID.");
  }
}

bool validarEstruturaJSON(const char* path) {
  File file = SD.open(path);
  if (!file) {
    Serial.println("❌ Não foi possível abrir o arquivo para validação.");
    return false;
  }

  int chaves = 0;  // Conta { e }
  int colchetes = 0;  // Conta [ e ]

  while (file.available()) {
    char c = file.read();
    if (c == '{') chaves++;
    else if (c == '}') chaves--;
    else if (c == '[') colchetes++;
    else if (c == ']') colchetes--;

    // Se em algum momento as contagens ficarem negativas, já está errado
    if (chaves < 0 || colchetes < 0) {
      file.close();
      return false;
    }
  }

  file.close();

  return (chaves == 0 && colchetes == 0);  // JSON bem balanceado
}

// lógicas para viajems ------------------------------------------------------------------------------------

// para salvar o ultimo limite no cartão sd
// para resolver o problema da demora de inicialização do gps
void salvarUltimoLimite() {
  delay(10);
  
  File file = SD.open("/ultimo_limite.txt", FILE_WRITE);
  if (file) {
    file.printf("%d,%d\n", vel_max, vel_max_chuva);
    file.flush();
    delay(10);
    file.close();
    Serial.printf("💾 Último limite salvo: %d / %d\n", vel_max, vel_max_chuva);
  } else {
    Serial.println("❌ Erro ao salvar último limite.");
  }
}

//caso o gps não tenha sido iniciado, carrega o ultimo limite
bool carregarUltimoLimite() {
  delay(10);
  
  File file = SD.open("/ultimo_limite.txt");
  if (file) {
    String linha = file.readStringUntil('\n');
    file.close();
    int v1, v2;
    if (sscanf(linha.c_str(), "%d,%d", &v1, &v2) == 2) {
      vel_max = v1;
      vel_max_chuva = v2;
      Serial.printf("📂 Limite carregado do SD: %d / %d\n", vel_max, vel_max_chuva);
      return true;
    }
  }
  return false;
}

void verificarCercas(float lat, float lng) {
  File file = SD.open("/cercas.json");
  if (!file) {
    Serial.println("Erro ao abrir cercas.json");
    return;
  }

  // Verifica se o JSON começa com [
  char c;
  do {
    c = file.read();
  } while (c != -1 && isspace(c)); // ignora espaços

  if (c != '[') {
    Serial.println("Formato inválido: JSON não começa com [");
    file.close();
    return;
  }

  StaticJsonDocument<1024> doc;
  bool encontrouAlguma = false;

  int menorVelMax = 999;         // Inicializa com valor alto
  int menorVelChuva = 999;
  const char* nomeMaisRestrito = nullptr;

  while (file.available()) {
    DeserializationError err = deserializeJson(doc, file);
    if (err) {
      Serial.print("Erro ao parsear objeto JSON: ");
      Serial.println(err.c_str());
      break;
    }

    JsonObject cerca = doc.as<JsonObject>();
    const char* nome = cerca["nome"];
    Serial.print("Verificando: ");
    Serial.println(nome);
    const char* velMaxStr = cerca["velocidade_max"];
    const char* velChuvaStr = cerca["velocidade_chuva"];
    JsonArray coords = cerca["coordenadas"];

    if (dentroDoPoligono(lat, lng, coords)) {
      int vel = atoi(velMaxStr);
      int velChuva = atoi(velChuvaStr);

      Serial.println("🛑 Dentro de uma cerca:");
      Serial.print("📍 Nome: "); Serial.println(nome);
      Serial.print("🚗 Limite: "); Serial.print(vel); Serial.println(" km/h");

      if (vel < menorVelMax) {
        menorVelMax = vel;
        menorVelChuva = velChuva;
        nomeMaisRestrito = nome;
        encontrouAlguma = true;
      }
    }

    bool terminou = false;
    while (file.available()) {
      char next = file.peek();
      if (next == ',') {
        file.read(); // consome vírgula
        break;
      } else if (isspace(next)) {
        file.read(); // consome espaço
      } else if (next == ']') {
        file.read(); // fim do array
        terminou = true;
        break;
      } else {
        break;
      }
    }

    if (terminou) break;

    doc.clear(); // limpa doc para o próximo
  }

  file.close();

  if (encontrouAlguma) {
    vel_max = menorVelMax;
    vel_max_chuva = menorVelChuva;

    lcd.clear();
    // lcd.setCursor(0, 0);
    // lcd.print(String(nomeMaisRestrito).substring(0, 16));

    lcd.setCursor(0, 1);
    lcd.print("Limite: ");
    lcd.print(vel_max);
    lcd.print("km/h");

    //para definir se estpa chovendo futuramente usando o sensor
    chuva = false;
  } else {
    Serial.println("📭 Fora de qualquer cerca.");
    lcd.clear();
    lcd.setCursor(0, 1);
    lcd.print("Fora de qualquer cerca");
  }
}

bool dentroDoPoligono(float x, float y, JsonArray coords) {
  bool dentro = false;
  int n = coords.size();
  
  for (int i = 0, j = n - 1; i < n; j = i++) {
    float xi = atof(coords[i][0]), yi = atof(coords[i][1]);
    float xj = atof(coords[j][0]), yj = atof(coords[j][1]);
    
    if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) {
      dentro = !dentro;
    }
  }
  return dentro;
}

// Funções para a lógica de registrar as posições --------------------------------------------------
String getTimestamp() {
  if (gps.date.isValid() && gps.time.isValid()) {
    char buffer[25];
    sprintf(buffer, "%04d-%02d-%02dT%02d:%02d:%02dZ",
            gps.date.year(),
            gps.date.month(),
            gps.date.day(),
            gps.time.hour(),
            gps.time.minute(),
            gps.time.second());
    return String(buffer);
  }
  return "0000-00-00T00:00:00Z";
}

unsigned long gerarIdUnico() {
    // Combina chip ID + timestamp + valor aleatório
    uint64_t chipId = ESP.getEfuseMac();
    uint32_t timestamp = millis();
    uint32_t randomVal = esp_random();
    
    unsigned long idUnico = ((chipId >> 32) ^ (chipId & 0xFFFFFFFF) ^ timestamp ^ randomVal);
    
    return idUnico;
}

//par recuperar viagens não finalizadas
bool primeiroRegistro = true;

void recuperarViagemInterrompida() {
  // Verificar se a pasta /pendente existe
  if (!SD.exists("/pendente")) {
    Serial.println("✅ Nenhuma viagem pendente");
    return;
  }

  File pendenteDir = SD.open("/pendente");
  if (!pendenteDir) {
    Serial.println("❌ Erro ao abrir pasta /pendente");
    return;
  }

  int viagensProcessadas = 0;
  
  // Processar todos os arquivos .json
  while (true) {
    File entry = pendenteDir.openNextFile();
    if (!entry) break;
    
    String nomeArquivo = String(entry.name());
    entry.close();

    // Pular diretórios e arquivos que não são .json
    if (nomeArquivo.equals(".") || nomeArquivo.equals("..") || 
        nomeArquivo.indexOf(".") == -1 || !nomeArquivo.endsWith(".json")) {
      continue;
    }

    String caminhoCompleto = "/pendente/" + nomeArquivo;
    
    Serial.print("🔄 Finalizando: ");
    Serial.println(nomeArquivo);

    // Adicionar "]}" para fechar o JSON
    File arquivo = SD.open(caminhoCompleto, FILE_APPEND);
    if (arquivo) {
      arquivo.print("]}");
      arquivo.flush();
      arquivo.close();
      
      // Mover para viagens
      String novoCaminho = "/viagens/" + nomeArquivo;
      if (SD.rename(caminhoCompleto, novoCaminho)) {
        Serial.print("✅ Movido: ");
        Serial.println(novoCaminho);
        viagensProcessadas++;
      } else {
        Serial.println("❌ Falha ao mover arquivo");
      }
    } else {
      Serial.println("❌ Erro ao abrir arquivo");
    }
  }

  pendenteDir.close();

  if (viagensProcessadas > 0) {
    Serial.print("✅ Processamento concluído. ");
    Serial.print(viagensProcessadas);
    Serial.println(" viagem(ns) recuperada(s)");
  } else {
    Serial.println("✅ Nenhuma viagem pendente encontrada");
  }
}

void iniciarViagem() {
  viagemId = gerarIdUnico();
  nomeArquivoViagem = "/pendente/viagem_" + String(viagemId) + ".json";

  arquivoViagem = SD.open(nomeArquivoViagem, FILE_WRITE);
  if (!arquivoViagem) {
    Serial.println("❌ Erro ao criar arquivo de viagem");
    return;
  }

  snprintf(jsonBuffer, sizeof(jsonBuffer),
    "{"
    "\"viagem_id\":%lu,"
    "\"motorista_id\":%d,"
    "\"veiculo_id\":%d,"
    "\"registros\":[\n",
    viagemId,
    motoristaAtual["id"].as<int>(),
    VEICULO_ID
  );

  arquivoViagem.print(jsonBuffer);
  arquivoViagem.flush();

  viagemAtiva = true;
  primeiroRegistro = true;

  Serial.print("✅ Viagem iniciada (Arquivo: ");
  Serial.print(nomeArquivoViagem);
  Serial.println(")");
}

void registrarPosicao(float lat, float lng, float vel, bool chuva) {
  if (!viagemAtiva) return;

  if (!primeiroRegistro) {
    arquivoViagem.print(",\n"); // adiciona vírgula entre registros
  } else {
    primeiroRegistro = false;
  }

  snprintf(jsonBuffer, sizeof(jsonBuffer),
    "{\"timestamp\":\"%s\",\"lat\":%.6f,\"lng\":%.6f,"
    "\"vel\":%.2f,\"chuva\":%s,\"lim_seco\":%d,\"lim_chuva\":%d}",
    getTimestamp().c_str(),
    lat, lng, vel,
    chuva ? "true" : "false",
    vel_max, vel_max_chuva
  );

  arquivoViagem.print(jsonBuffer);
  arquivoViagem.flush();
  salvarUltimoLimite();

  Serial.print("Posição registrada, velocidade: ");
  Serial.println(vel);
}

void encerrarViagem() {
  if (!viagemAtiva || nomeArquivoViagem == "") {
    Serial.println("❌ Viagem não está ativa para encerrar");
    return;
  }

  // Verificar se o arquivo ainda está aberto e válido
  if (!arquivoViagem) {
    Serial.println("❌ Arquivo de viagem não está aberto");
    viagemAtiva = false;
    return;
  }

  Serial.println("🔄 Finalizando viagem...");

  // Registrar última posição apenas se o GPS estiver ativo
  if (gps.location.isValid()) {
    float lat = gps.location.lat();
    float lng = gps.location.lng();
    float vel = gps.speed.isValid() ? gps.speed.kmph() : 0.0;
    
    if (!primeiroRegistro) {
      arquivoViagem.print(",\n");
    } else {
      primeiroRegistro = false;
    }

    snprintf(jsonBuffer, sizeof(jsonBuffer),
      "{\"timestamp\":\"%s\",\"lat\":%.6f,\"lng\":%.6f,"
      "\"vel\":%.2f,\"chuva\":false,\"lim_seco\":%d,\"lim_chuva\":%d}",
      getTimestamp().c_str(),
      lat, lng, vel,
      vel_max, vel_max_chuva
    );

    arquivoViagem.print(jsonBuffer);
    arquivoViagem.flush();
    Serial.println("✅ Última posição registrada");
  }

  // Fechar o JSON
  arquivoViagem.print("]}");
  arquivoViagem.flush();
  arquivoViagem.close();
  Serial.println("✅ JSON fechado");

  iniciarEnvioViagens();
  // Mover arquivo
  String novoNome = "/viagens/viagem_" + String(viagemId) + ".json";
  if (SD.rename(nomeArquivoViagem, novoNome)) {
    Serial.print("✅ Viagem movida para: ");
    Serial.println(novoNome);
  } else {
    Serial.print("❌ Erro ao mover, mantendo em: ");
    Serial.println(nomeArquivoViagem);
  }

  // Resetar variáveis
  viagemAtiva = false;
  nomeArquivoViagem = "";
  viagemId = 0;
  primeiroRegistro = true;
  
  Serial.println("✅ Viagem encerrada com sucesso");
}
