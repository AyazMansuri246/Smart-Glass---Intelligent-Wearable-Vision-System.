## ESP32-CAM Module

Handles image capture and video recording using onboard camera.
Responsible to store data and pass on to mobile app.

### Responsibilities:

- Capture image and video media and store it to SD card and make it available to mobile app.
- Also store audio coming from microphone attached to esp-wroom.

### Notes:

- Uses ESP32-CAM (AI Thinker)
- Communication can be via WiFi / Serial (experimental)
- Uses ESP-NOW protocol to communicate with esp32 wroom.
- Also Uses UART serial connection to send output to esp-wroom to show on OLED display.
- It also uses WebSocket connection to send and receive live audio and text data to flutter app.
