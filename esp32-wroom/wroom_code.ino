// ESP32-WROOM: Output Control (Sample Code)

#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32-WROOM Ready");
}

void loop() {
  
  String receivedData = "Detected: Object / Speech Output";

  // Process result
  Serial.println("Processing AI Output:");
  Serial.println(receivedData);


  delay(3000);
}