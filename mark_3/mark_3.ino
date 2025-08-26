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
StaticJsonDocument<512> motoristaAtual;
bool motoristaEncontrado = false;

// === GPS ===
#define RX_GPS 16
#define TX_GPS -1
HardwareSerial gpsSerial(2);  // UART2
TinyGPSPlus gps;

bool gpsAtivo = false;

//coordenadas de origem
//necessárias no registro, mas o GPS demora para iniciar
//isso garante que elas não fiquem vazias
// double origemLat = 0.0;
// double origemLng = 0.0;
// bool origemDefinida = false;

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
unsigned long ultimaAtualizacao = 0;
const unsigned long intervaloAtualizacao = 5 * 60 * 1000; // 5 minutos
unsigned long ultimaVerificacaoCercas = 0;
const unsigned long intervaloVerificacaoCercas = 5000; // 5 segundos

int vel_max;
int vel_max_chuva;

// === RFID ===
#define RST_PIN 27
#define SS_PIN_RFID 15

MFRC522 mfrc522(SS_PIN_RFID, RST_PIN);
SPIClass spiRFID(VSPI);  // VSPI para RFID

boolean rfidLido = false;
String rfidValor;
String ultimoUID = "";

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
const String VEICULO_IDENTIFICADOR = "VEICULO_2";
const String VEICULO_MODELO = "Toyota Hilux";

//prototipo das funções
void verificarMotoristaPorRFID();
void salvarUltimoLimite();
void encerrarViagem(float destinoLat, float destinoLng);



unsigned long ultimaLeituraRFID = 0;
const unsigned long DEBOUNCE_RFID = 1000;
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

    if (millis() - ultimaLeitura > 10000) { // 10s sem resposta
      resetRFID();
      ultimaLeitura = millis();
    }

    // if (millis() - ultimaVerificacao > 10000) { // a cada 10s
    //   byte ver = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
    //   Serial.print("RFID Version check: ");
    //   Serial.println(ver, HEX);

    //   if (ver == 0x00 || ver == 0xFF) {
    //     Serial.println("⚠️ Versão inválida detectada, resetando RFID...");
    //     resetRFID();
    //   }
    //   ultimaVerificacao = millis();
    // }

    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void resetRFID() {
  Serial.println("⚠️ Resetando módulo RFID...");
  digitalWrite(RST_PIN, LOW);
  delay(100);
  digitalWrite(RST_PIN, HIGH);
  delay(100);
  SPI.end();
  SPI.begin(14, 12, 13, SS_PIN_RFID); 
  mfrc522.PCD_Init();  // Re-inicializa o RC522
  byte ver = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  Serial.print("RFID Version após reset: ");
  Serial.println(ver, HEX);
}

//------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

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
  // while (WiFi.status() != WL_CONNECTED && millis() - inicioWifi < 10000) {
  //   delay(500);
  //   Serial.print(".");
  // }

  // Serial.println(WiFi.status() == WL_CONNECTED ? "\nWi-Fi conectado!" : "\n⚠️ Wi-Fi não disponível. Usando dados locais.");

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

  // if (WiFi.status() == WL_CONNECTED) {
  //   atualizarCercas();
  //   atualizarMotoristas();
  // }

  xTaskCreatePinnedToCore(
    taskRFID,     // Função da tarefa
    "RFID Reader",// Nome da tarefa
    4096,         // Tamanho da stack
    NULL,         // Parâmetro
    1,            // Prioridade
    NULL,         // Handle da tarefa
    0             // Core 0 (o loop() roda no Core 1)
  );

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Iniciando gps...");
}

//------------------------------------------------------------------------

void loop() {
  if (millis() - ultimaAtualizacao > intervaloAtualizacao) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("🔌 Wi-Fi não conectado. Tentando reconectar...");
      WiFi.begin(ssid, password);
      unsigned long inicio = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - inicio < 10000) {
        delay(500);
        Serial.print(".");
      }
      Serial.println(WiFi.status() == WL_CONNECTED ? "\n✅ Reconectado ao Wi-Fi!" : "\n❌ Falha ao reconectar.");
    }

    if (WiFi.status() == WL_CONNECTED) {
      atualizarCercas();
      atualizarMotoristas();
    }

    ultimaAtualizacao = millis();
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


  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());

    if (gps.location.isUpdated()) {
      gpsAtivo = true;

      float lat = gps.location.lat();
      float lng = gps.location.lng();
      float vel = gps.speed.kmph(); // velocidade do obd2
      bool chuva = false; // sensor IR

      // registra a primeira coordenada recebida do GPS como coordenada de origem
      // if (!origemDefinida && gps.location.isValid()) {
      //   origemLat = lat;
      //   origemLng = lng;
      //   origemDefinida = true;

      //   Serial.print("✅ Origem registrada: ");
      //   Serial.print(origemLat, 6);
      //   Serial.print(", ");
      //   Serial.println(origemLng, 6);

      // }

      if (millis() - ultimaVerificacaoCercas > intervaloVerificacaoCercas) {
        Serial.print("Lat: "); Serial.println(lat, 6);
        Serial.print("Lng: "); Serial.println(lng, 6);
        ultimaVerificacaoCercas = millis();

        verificarCercas(lat, lng);
        registrarPosicao(lat, lng, vel, chuva);
      }
    }
  }
  } else {

    if (!lcdFlag) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Inicie a Viagem");
      lcdFlag = true;
    }
  }
}

//------------------------------------------------------------------------

void atualizarCercas() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  if (https.begin(client, apiURL)) {
    https.addHeader("User-Agent", "ESP8266");
    https.addHeader("Accept", "application/json");
    https.addHeader("Accept-Encoding", "identity");
    https.setTimeout(10000); // 10 segundos de espera de resposta da API


    int httpCode = https.GET();
    if (httpCode == 200) {
      Serial.println("Resposta OK. Salvando no arquivo temporário...");

      // Salva em arquivo temporário
      File temp = SD.open("/temp_cercas.json", FILE_WRITE);
      if (temp) {
        WiFiClient& stream = https.getStream();

        unsigned long inicio = millis();
        const unsigned long tempoLimite = 10000; // 10 segundos
        bool iniciouJson = false;

        while ((millis() - inicio) < tempoLimite) {
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

            inicio = millis(); // Reinicia tempo sempre que lê algo
          }
        }

        temp.flush();
        temp.close();
        Serial.println("✅ Arquivo temporário salvo com sucesso!");

        if (validarEstruturaJSON("/temp_cercas.json")) {
          Serial.println("✅ JSON parece válido! Substituindo arquivo oficial...");

          if (SD.exists("/cercas.json")) {
            SD.remove("/cercas.json");
          }

          SD.rename("/temp_cercas.json", "/cercas.json");
          Serial.println("📝 Substituição concluída.");
        } else {
          Serial.println("⚠️ JSON inválido (estrutura incompleta). Mantendo arquivo antigo.");
        }

      } else {
        Serial.println("❌ Erro ao abrir arquivo temporário para escrita.");
      }
    } else {
      Serial.print("⚠️ Falha na requisição. Código HTTP: ");
      Serial.println(httpCode);
    }
    
    https.end();

  } else {
    Serial.println("❌ Erro ao iniciar conexão HTTPS.");
  }
}

void atualizarMotoristas() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  if (https.begin(client, apiMotoristas)) {
    https.addHeader("User-Agent", "ESP8266");
    https.addHeader("Accept", "application/json");
    https.addHeader("Accept-Encoding", "identity");
    https.setTimeout(10000);

    int httpCode = https.GET();
    if (httpCode == 200) {
      Serial.println("🔄 Baixando motoristas...");

      File temp = SD.open("/temp_motoristas.json", FILE_WRITE);
      if (temp) {
        WiFiClient& stream = https.getStream();
        unsigned long inicio = millis();
        const unsigned long tempoLimite = 10000;
        bool iniciouJson = false;

        while ((millis() - inicio) < tempoLimite) {
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
          }
        }

        temp.flush();
        temp.close();
        Serial.println("✅ Motoristas temporários salvos.");

        if (validarEstruturaJSON("/temp_motoristas.json")) {
          Serial.println("🧾 JSON de motoristas válido.");

          if (SD.exists("/motoristas.json")) {
            SD.remove("/motoristas.json");
          }
          SD.rename("/temp_motoristas.json", "/motoristas.json");
          Serial.println("📦 motoristas.json atualizado com sucesso.");
        } else {
          Serial.println("❌ JSON de motoristas inválido.");
        }

      } else {
        Serial.println("❌ Erro ao abrir temp_motoristas.json.");
      }
    } else {
      Serial.printf("Erro HTTP: %d\n", httpCode);
    }

    https.end();
  } else {
    Serial.println("❌ Erro ao iniciar conexão com motoristas.");
  }
}

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

  StaticJsonDocument<512> doc;

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

  StaticJsonDocument<2048> doc;
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
    lcd.setCursor(0, 0);
    lcd.print(String(nomeMaisRestrito).substring(0, 16));

    lcd.setCursor(0, 1);
    lcd.print("Limite: ");
    lcd.print(vel_max);
    lcd.print("km/h");
  } else {
    Serial.println("📭 Fora de qualquer cerca.");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Fora de qualquer cerca");
  }
}

bool dentroDoPoligono(float x, float y, JsonArray coords) {
  int i, j, n = coords.size();
  bool dentro = false;

  for (i = 0, j = n - 1; i < n; j = i++) {
    float xi = atof(coords[i][0]), yi = atof(coords[i][1]);
    float xj = atof(coords[j][0]), yj = atof(coords[j][1]);

    if (((yi > y) != (yj > y)) &&
        (x < (xj - xi) * (y - yi) / (yj - yi + 1e-9) + xi)) {
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

void iniciarViagem() {
  delay(50);
  
  nomeArquivoViagem = "/viagens/viagem_" + String(millis()) + ".json";
  arquivoViagem = SD.open(nomeArquivoViagem, FILE_WRITE);

  if (!arquivoViagem) {
    Serial.println("❌ Erro ao criar arquivo de viagem");
    return;
  }
  
  // Cabeçalho simplificado - primeira linha
  arquivoViagem.print("{\"motorista_id\":");
  arquivoViagem.print(motoristaAtual["id"].as<int>());
  arquivoViagem.print(",\"veiculo_id\":");
  arquivoViagem.print(VEICULO_ID);
  arquivoViagem.print(",\"inicio\":\"");
  arquivoViagem.print(getTimestamp());
  arquivoViagem.print("\"}");
  arquivoViagem.println(); // Nova linha para o próximo registro
  
  arquivoViagem.flush();
  delay(10);
  
  viagemAtiva = true;
  Serial.println("✅ Cabeçalho da viagem criado");
}

void registrarPosicao(float lat, float lng, float vel, bool chuva) {
  if (!viagemAtiva) return;
  
  delay(10);
  
  arquivoViagem.print("{\"timestamp\":\"");
  arquivoViagem.print(getTimestamp());
  arquivoViagem.print("\",\"lat\":");
  arquivoViagem.print(lat, 6);
  arquivoViagem.print(",\"lng\":");
  arquivoViagem.print(lng, 6);
  arquivoViagem.print(",\"vel\":");
  arquivoViagem.print(vel, 2);
  arquivoViagem.print(",\"chuva\":");
  arquivoViagem.print(chuva ? "true" : "false");
  arquivoViagem.print(",\"lim_seco\":");
  arquivoViagem.print(vel_max);
  arquivoViagem.print(",\"lim_chuva\":");
  arquivoViagem.print(vel_max_chuva);
  arquivoViagem.print("}");
  arquivoViagem.println(); // Nova linha para o próximo registro
  
  arquivoViagem.flush();
  salvarUltimoLimite();
  delay(10);
}

void encerrarViagem() {
  if (!viagemAtiva) return;

  delay(20);
  
  // Última linha com coordenadas finais
  arquivoViagem.print("{\"fim\":\"");
  arquivoViagem.print(getTimestamp());
  arquivoViagem.print("\",\"dest_lat\":");
  arquivoViagem.print(gps.location.isValid() ? gps.location.lat() : 0.0, 6);
  arquivoViagem.print(",\"dest_lng\":");
  arquivoViagem.print(gps.location.isValid() ? gps.location.lng() : 0.0, 6);
  arquivoViagem.print("}");
  arquivoViagem.println();
  
  arquivoViagem.flush();
  delay(20);
  
  arquivoViagem.close();
  delay(10);
  
  viagemAtiva = false;
  // origemDefinida = false;
  Serial.println("✅ Viagem encerrada");
}



