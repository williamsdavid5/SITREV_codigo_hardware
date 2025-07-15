#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <WiFiClientSecure.h>
#include <TinyGPSPlus.h>
#include <SoftwareSerial.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// === Cartão SD ===
#define SD_CS 15  // D8 (GPIO15)

// === GPS ===
#define RX_GPS 0  // D3 (GPIO0)
#define TX_GPS -1
SoftwareSerial gpsSerial(RX_GPS, TX_GPS);
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

void setup() {
  Serial.begin(115200);
  gpsSerial.begin(9600);

  pinMode(2, OUTPUT);
  digitalWrite(2, 0);

  Wire.begin(4, 5);
  lcd.begin(16, 2);
  lcd.backlight();
  lcd.clear();

  // Conecta ao Wi-Fi (com timeout)
  Serial.println("Conectando ao Wi-Fi...");
  WiFi.begin(ssid, password);
  
  unsigned long inicioWifi = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicioWifi < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi conectado!");
  } else {
    Serial.println("\n⚠️ Wi-Fi não disponível. Usando dados locais.");
  }

  // Inicia SD
  if (!SD.begin(SD_CS)) {
    Serial.println("❌ Falha ao iniciar o cartão SD!");
    return;
  }

  // Tenta atualizar dados (caso haja conexão)
  if (WiFi.status() == WL_CONNECTED) {
    atualizarCercas();
  }

  lcd.setCursor(0, 0);
  lcd.print("Iniciando gps...");
}

void loop() {
  // Atualização periódica (a cada 5 minutos)
  if (millis() - ultimaAtualizacao > intervaloAtualizacao) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("🔌 Wi-Fi não conectado. Tentando reconectar...");
      WiFi.begin(ssid, password);
      unsigned long inicio = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - inicio < 10000) {
        delay(500);
        Serial.print(".");
      }
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✅ Reconectado ao Wi-Fi!");
      } else {
        Serial.println("\n❌ Falha ao reconectar.");
      }
    }

    if (WiFi.status() == WL_CONNECTED) {
      atualizarCercas();
    }

    ultimaAtualizacao = millis();
  }

  // GPS funcionando normalmente
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());

    if (gps.location.isUpdated()) {
      float lat = gps.location.lat();
      float lng = gps.location.lng();

      Serial.print("Lat: "); Serial.println(lat, 6);
      Serial.print("Lng: "); Serial.println(lng, 6);

      verificarCercas(lat, lng);
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

        // Agora validamos o JSON do arquivo temporário
        File tempFile = SD.open("/temp_cercas.json");
        StaticJsonDocument<8192> doc;
        DeserializationError err = deserializeJson(doc, tempFile);
        tempFile.close();

        if (!err) {
          Serial.println("✅ JSON válido! Substituindo arquivo oficial...");

        if (SD.exists("/cercas.json")) {
          SD.remove("/cercas.json");
        }

        SD.rename("/temp_cercas.json", "/cercas.json");
        Serial.println("📝 Substituição concluída.");
        } else {
          Serial.print("⚠️ JSON inválido: ");
          Serial.println(err.c_str());
          Serial.println("❌ Mantendo o arquivo antigo. Temporário será mantido para análise.");
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

void verificarCercas(float lat, float lng) {
  File file = SD.open("/cercas.json");
  if (!file) {
    Serial.println("Erro ao abrir cercas.json");
    return;
  }

  StaticJsonDocument<8192> doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();

  if (err) {
    Serial.print("Erro ao parsear JSON: ");
    Serial.println(err.c_str());
    return;
  }

  for (JsonObject cerca : doc.as<JsonArray>()) {
    const char* nome = cerca["nome"];
    const char* velMax = cerca["velocidade_max"];
    JsonArray coords = cerca["coordenadas"];

    if (dentroDoPoligono(lat, lng, coords)) {
      Serial.println("🛑 Dentro de uma cerca:");
      Serial.print("📍 Nome: "); Serial.println(nome);
      Serial.print("🚗 Limite: "); Serial.print(velMax); Serial.println(" km/h");

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(String(nome).substring(0, 16));

      lcd.setCursor(0, 1);
      lcd.print("Limite: ");
      lcd.print(velMax);
      lcd.print("km/h");
      break;
    }
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