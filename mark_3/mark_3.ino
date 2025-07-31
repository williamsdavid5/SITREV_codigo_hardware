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

// === Cartão SD ===
#define SD_CS 13

// === GPS ===
#define RX_GPS 16
#define TX_GPS -1
HardwareSerial gpsSerial(2);  // UART2
TinyGPSPlus gps;

// === LCD ===
LiquidCrystal_I2C lcd(0x27, 16, 2);

// === Wi-Fi e API ===
const char* ssid = "Duarte_Fotos";
const char* password = "05519558213";
const char* apiURL = "https://telemetria-fvv4.onrender.com/cercas";

// === Atualização periódica ===
unsigned long ultimaAtualizacao = 0;
const unsigned long intervaloAtualizacao = 5 * 60 * 1000; // 5 minutos
unsigned long ultimaVerificacaoCercas = 0;
const unsigned long intervaloVerificacaoCercas = 5000; // 5 segundos

int vel_max;
int vel_max_chuva;

void setup() {
  Serial.begin(115200);
  gpsSerial.begin(9600, SERIAL_8N1, RX_GPS, TX_GPS);

  pinMode(2, OUTPUT);
  digitalWrite(2, 0);

  Wire.begin(4, 5);
  lcd.begin(16, 2);
  lcd.backlight();
  lcd.clear();

  Serial.println("Conectando ao Wi-Fi...");
  WiFi.begin(ssid, password);
  unsigned long inicioWifi = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicioWifi < 10000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println(WiFi.status() == WL_CONNECTED ? "\nWi-Fi conectado!" : "\n⚠️ Wi-Fi não disponível. Usando dados locais.");

  if (!SD.begin(SD_CS)) {
    Serial.println("❌ Falha ao iniciar o cartão SD!");
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    atualizarCercas();
  }

  lcd.setCursor(0, 0);
  lcd.print("Iniciando gps...");
}

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
    }

    ultimaAtualizacao = millis();
  }

  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());

    if (gps.location.isUpdated()) {
      float lat = gps.location.lat();
      float lng = gps.location.lng();

      if (millis() - ultimaVerificacaoCercas > intervaloVerificacaoCercas) {
        Serial.print("Lat: "); Serial.println(lat, 6);
        Serial.print("Lng: "); Serial.println(lng, 6);
        ultimaVerificacaoCercas = millis();
        verificarCercas(lat, lng);
      }
    }
  }
}

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
  }

    https.end();
  } else {
    Serial.println("❌ Erro ao iniciar conexão HTTPS.");
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