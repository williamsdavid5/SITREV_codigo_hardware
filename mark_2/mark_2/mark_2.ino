#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <WiFiClientSecure.h>
#include <TinyGPSPlus.h>
#include <SoftwareSerial.h>
#include <ArduinoJson.h>
// #include <Wire.h>
// #include <LiquidCrystal_I2C.h>
// #include <MFRC522.h>

// === Cartão SD ===
#define SD_CS 15  // D8 (GPIO15)

// === GPS ===
#define RX_GPS 0  // D3 (GPIO0)
#define TX_GPS -1
SoftwareSerial gpsSerial(RX_GPS, TX_GPS);
TinyGPSPlus gps;

// === LCD ===
// LiquidCrystal_I2C lcd(0x27, 16, 2);

// //leitor RFID
// #define RST_PIN 4   // D2
// #define SS_PIN 5     // D1
// MFRC522 rfid(SS_PIN, RST_PIN);
// bool cartaoDetectado = false;

// === Wi-Fi e API ===
// === Redes Wi-Fi disponíveis ===

const char* apiURL = "https://telemetria-fvv4.onrender.com/cercas";

struct RedeWiFi {
  const char* ssid;
  const char* password;
};

// Adicione ou remova redes facilmente aqui:
RedeWiFi redesWiFi[] = {
  {"Duarte_Fotos", "05519558213"},
  {"OutraRede", "senha12345"}
};
const int numRedes = sizeof(redesWiFi) / sizeof(redesWiFi[0]);

// === Atualização periódica ===
unsigned long ultimaAtualizacao = 0;
const unsigned long intervaloAtualizacao = 5 * 60 * 1000; // 5 minutos

// Para evitar verificações de cerca a cada segundo
unsigned long ultimaVerificacaoCercas = 0;
const unsigned long intervaloVerificacaoCercas = 5000; // 5 segundos

//valores de velocidade para uso futuro
int vel_max;
int vel_max_chuva;

void lcdPrint(String texto);
void atualizarCercas();
void verificarCercas(float latitude, float longitude);
bool tentarConectarWiFi(int tempoLimiteMs = 10000);

void setup() {
  Serial.begin(115200);
  gpsSerial.begin(9600);

  // pinMode(2, OUTPUT);
  // digitalWrite(2, 0);

  // Wire.begin(4, 5);
  // lcd.begin(16, 2);
  // lcd.backlight();
  // lcd.clear();
  lcdPrint("Iniciando...");

  SPI.begin();

  Serial.println("Conectando ao Wi-Fi...");
  bool conectado = tentarConectarWiFi();

  if (conectado) {
    Serial.println("📶 Wi-Fi conectado.");
  } else {
    Serial.println("⚠️ Nenhuma rede disponível. Usando dados locais.");
  }

  if (!SD.begin(SD_CS)) {
    Serial.println("❌ Falha ao iniciar o cartão SD!");
    lcdPrint("Falha ao iniciar o cartao SD");
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    atualizarCercas();
  }

  lcdPrint("Iniciando GPS...");
}

void loop() {
  if (millis() - ultimaAtualizacao > intervaloAtualizacao) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("🔌 Wi-Fi não conectado. Tentando reconectar...");

      bool reconectado = tentarConectarWiFi();
      if (reconectado) {
        Serial.println("✅ Reconectado ao Wi-Fi!");
      } else {
        Serial.println("❌ Falha ao reconectar.");
      }
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


bool tentarConectarWiFi(int tempoLimiteMs) {
  for (int i = 0; i < numRedes; i++) {
    Serial.print("Tentando conectar em: ");
    Serial.println(redesWiFi[i].ssid);
    WiFi.begin(redesWiFi[i].ssid, redesWiFi[i].password);

    unsigned long inicio = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - inicio < tempoLimiteMs) {
      delay(500);
      Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n✅ Conectado à rede: " + String(redesWiFi[i].ssid));
      return true;
    } else {
      Serial.println("\n❌ Falha na conexão com: " + String(redesWiFi[i].ssid));
    }
  }

  return false;
}

void lcdPrint(String texto) {
  Serial.println("[LCD]" + texto + "[LCD]");  // envia comando para o Arduino
}

// void lcdPrint(String textoLcd) {
//   lcd.clear();

//   String linha1 = textoLcd.substring(0, 16);
//   String linha2 = "";

//   if (textoLcd.length() > 16) {
//     linha2 = textoLcd.substring(16, 32);
//   }

//   lcd.setCursor(0, 0);
//   lcd.print(linha1);

//   lcd.setCursor(0, 1);
//   lcd.print(linha2);
// }


void atualizarCercas() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  if (https.begin(client, apiURL)) {
    https.addHeader("User-Agent", "ESP8266");
    https.addHeader("Accept", "application/json");
    https.addHeader("Accept-Encoding", "identity");
    https.setTimeout(10000); // 10 segundos de espera de resposta da API

    Serial.println("Eviando requisição...");

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

    // lcd.clear();
    // lcd.setCursor(0, 0);
    // lcd.print(String(nomeMaisRestrito).substring(0, 16));

    // lcd.setCursor(0, 1);
    // lcd.print("Limite: ");
    // lcd.print(vel_max);
    // lcd.print("km/h");

    Serial.print("Cerca: ");
    Serial.println(nomeMaisRestrito);
    Serial.print(vel_max);
    Serial.println(" km/h");
  } else {
    Serial.println("📭 Fora de qualquer cerca.");
    lcdPrint("Fora de qualquer cerca...");
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