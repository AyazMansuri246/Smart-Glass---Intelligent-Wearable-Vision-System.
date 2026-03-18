# Mobile Inference Pipeline

The mobile device acts as the primary computation unit for AI inference, translation task and media access.

## Pipeline Flow

Input is given through touch gesture on TP4056 module touch sensor.
Double tap- Capture Image, storing on SD card attach to esp32 cam.
Long Press- Record Video including Audio through microphone and merging on mobile phone through "ffmpeg" Library.
Triple Tap- Live Audio Translation.

1. ESP32-CAM captures image/video and esp32 wroom captures audio.
2. Data is sent to mobile device
3. Mobile performs:
   - Speech Recognition (ASR)
   - Translation using google ML Kit library (best option for offline)
   - Media access and download
4. Processed output/translated text is sent to ESP32-WROOM via ESP32 CAM
5. ESP32-WROOM outputs text on display.

## Why Mobile?

- Higher compute power than ESP32
- Enables offline AI processing
- Avoids cloud dependency (privacy + latency benefits)

## Models Explored

- Vosk (lightweight ASR)
- Whisper (higher accuracy, higher resource usage)
- Google ML Kit Library
- Also Exploring YOLO model for mobile inference

## Focus Areas

- Fully Offline
- Low latency inference
- Memory-efficient models
- Real-time processing
