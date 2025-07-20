#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SoftwareSerial.h>

SoftwareSerial espSerial(3, 4); // RX = D3, TX = D4 (ESP TX → D3)
LiquidCrystal_I2C lcd(0x27, 16, 2);

void setup() {
  espSerial.begin(115200);
  Serial.begin(115200);
  lcd.begin(16, 2);
  lcd.backlight();
  lcd.clear();
  lcd.print("LCD Iniciado.");
}

void loop() {
  static String buffer = "";

  while (espSerial.available()) {
    char c = espSerial.read();
    buffer += c;

    if (c == '\n') {
      if (buffer.startsWith("[LCD]")) {
        String texto = buffer.substring(5);
        Serial.print(texto);
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(texto.substring(0, 16));
        if (texto.length() > 16) {
          lcd.setCursor(0, 1);
          lcd.print(texto.substring(16, 32));
        }
      }
      buffer = "";
    }
  }
}
