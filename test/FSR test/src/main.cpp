#include <Arduino.h>

int fsrPin = 2;  // XIAO ESP32C3 的 D0

void setup() {
  Serial.begin(115200);
}

void loop() {
  int value = analogRead(fsrPin);
  Serial.println(value);
  delay(100);
}