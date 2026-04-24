#include <Arduino.h>

void printSensorStatus(int pin, String name) {
  int val = analogRead(pin);
  float voltage = val * 3.3 / 4095.0;

  Serial.print(name);
  Serial.print(" | Voltage: ");
  Serial.print(voltage, 3);
  Serial.print("V  →  ");

  if (voltage > 2.5) {
    Serial.println("A come！");
  } else if (voltage < 0.8) {
    Serial.println("B come！");
  } else {
    Serial.println("empty");
  }
}

void setup() {
  Serial.begin(115200);
}

void loop() {
  printSensorStatus(A0, "Sensor1");
  printSensorStatus(A1, "Sensor2");
  Serial.println("---");
  delay(500);
}