#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SoftwareSerial.h>

// RX do Arduino (D3) ← TX do ESP8266 | TX do Arduino (D4) → RX do ESP8266 (com divisor de tensão!)
SoftwareSerial espSerial(3, 4);

// Endereço I2C 0x27, LCD 16x2
LiquidCrystal_I2C lcd(0x27, 16, 2);

void setup() {
  espSerial.begin(115200);
  Serial.begin(115200);

  lcd.init();            // Inicializa usando a lib correta
  lcd.backlight();       // Liga a luz de fundo
  lcd.clear();
  lcd.setCursor(0, 0);.
}

void loop() {
  static String buffer = "";

  while (espSerial.available()) {
    char c = espSerial.read();
    buffer += c;

    if (c == '\n') {
      int inicio = buffer.indexOf("[LCD]");
      int fim = buffer.lastIndexOf("[LCD]");

      if (inicio != -1 && fim != -1 && fim > inicio) {
        String texto = buffer.substring(inicio + 5, fim); // Extrai só o que está entre os dois [LCD]
        Serial.print(texto);
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(texto.substring(0, 16)); // Primeira linha
        if (texto.length() > 16) {
          lcd.setCursor(0, 1);
          lcd.print(texto.substring(16, 32)); // Segunda linha
        }
      }

      buffer = ""; // Limpa o buffer mesmo se a estrutura estiver incorreta
    }
  }
}
