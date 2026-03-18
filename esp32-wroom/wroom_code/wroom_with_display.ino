// Touch Sensor, ESP-NOW communication, I2S mic input, and OLED display code for ESP32 WROOM  

#include <WiFi.h>
#include <esp_now.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// for differentiating between the audio and command
#define PKT_CONTROL 0x01
#define PKT_AUDIO 0x02

#define RXD2 16
#define TXD2 17


#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64


Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Bitmap buffer
uint8_t imageBuffer[1024];

// UART image receiver state
bool receivingImage = false;
bool newImageReady = false;
int imageIndex = 0;

// for mic
#define I2S_WS 25
#define I2S_SCK 26
#define I2S_SD 33

#define SAMPLE_RATE 16000
#define RECORD_SECONDS 30
#define BYTES_PER_SEC (SAMPLE_RATE * 2)
#define TOTAL_BYTES (BYTES_PER_SEC * RECORD_SECONDS)

#define CHUNK_SIZE 244  // bytes per ESP-NOW packet

float currentGain = 4.0;
#define TARGET_LEVEL 8000
#define MAX_GAIN 4.0
#define MIN_GAIN 1.5

bool liveMode = false;  // true when triple tap active


int16_t audioSamples[CHUNK_SIZE / 2];

size_t totalSent = 0;

uint16_t audioPacketSeq = 0;


// -------- TOUCH CONFIG --------
#define TOUCH_PIN 13
#define DEBOUNCE_TIME 50
#define DOUBLE_TAP_TIME 400
#define LONG_PRESS_TIME 2000
#define CMD_TRIPLE_TAP "TRIPLE_TAP"


bool isTouching = false;
bool isRecording = false;
unsigned long touchStartTime = 0;
unsigned long lastTouchChange = 0;
uint8_t tapCount = 0;
unsigned long lastTapTime = 0;


// -------- CAM MAC (CHANGE THIS) --------
// ESP32 cam MAC: 88:57:21:C3:9C:28
uint8_t camMAC[] = { 0x88, 0x57, 0x21, 0xC3, 0x9C, 0x28 };

// -------- Message structure --------
typedef struct {
  char cmd[16];
} ControlMsg;


// -------- Receive callback (ACKs) --------
void onReceive(const esp_now_recv_info* info,
               const uint8_t* incomingData,
               int len) {

  ControlMsg msg;
  memcpy(&msg, incomingData, sizeof(msg));

  Serial.print("CAM says: ");
  Serial.println(msg.cmd);

  // ---------- LIVE START CONFIRM ----------

  if (strcmp(msg.cmd, "LIVE_STARTED") == 0) {
    Serial.println("Live mode confirmed by CAM");
    liveMode = true;
    isRecording = true;
    totalSent = 0;
    audioPacketSeq = 0;
  }

  else if (strcmp(msg.cmd, "LIVE_STOPPED") == 0) {
    Serial.println("Received Stop command (from cam)..");
    liveMode = false;
    isRecording = false;
    totalSent = 0;
  }

  // ---------- APP NOT READY ----------
  else if (strcmp(msg.cmd, "APP_NOT_READY") == 0) {
    Serial.println("App not connected");
    liveMode = false;
    isRecording = false;
  }
}


// -------- Init ESP-NOW (blocking) --------
void initEspNow() {
  while (true) {
    if (esp_now_init() == ESP_OK) {
      Serial.println("ESP-NOW initialized");
      break;
    }
    Serial.println("ESP-NOW init failed, retrying...");
    delay(1000);
  }
}

// -------- Add peer (blocking) --------
void addPeer() {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, camMAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  while (true) {
    if (esp_now_add_peer(&peerInfo) == ESP_OK) {
      Serial.println("Peer added");
      break;
    }
    Serial.println("Peer add failed, retrying...");
    delay(1000);
  }
}

// -------- Send command --------
void sendCmd(const char* cmd) {
  uint8_t packet[1 + sizeof(ControlMsg)];
  packet[0] = PKT_CONTROL;

  ControlMsg msg;
  memset(&msg, 0, sizeof(msg));
  strncpy(msg.cmd, cmd, sizeof(msg.cmd) - 1);

  memcpy(packet + 1, &msg, sizeof(msg));
  esp_now_send(camMAC, packet, sizeof(packet));
}

bool waitingForSecondTap = false;
unsigned long firstTapTime = 0;



//for live translation
void handleTripleTap() {

  if (liveMode) {
    Serial.println("Stopping Live Mode (WROOM)");

    // liveMode = false;
    // isRecording = false;
    // totalSent = 0;

    sendCmd("TRIPLE_TAP");  // notify CAM
  } else {
    Serial.println("Starting Live Mode (WROOM)");

    // liveMode = true;
    // isRecording = true;
    // totalSent = 0;
    // audioPacketSeq = 0;

    sendCmd("TRIPLE_TAP");  // notify CAM
  }
}

// -------- Touch logic --------
void handleTouch() {

  if (millis() - lastTouchChange < DEBOUNCE_TIME) return;

  bool touchState = digitalRead(TOUCH_PIN);

  // ---------- TOUCH START ----------
  if (touchState && !isTouching) {
    isTouching = true;
    touchStartTime = millis();
    lastTouchChange = millis();
  }

  // ---------- TOUCH RELEASE ----------
  if (!touchState && isTouching) {
    isTouching = false;
    lastTouchChange = millis();

    unsigned long pressDuration = millis() - touchStartTime;

    // ===== LONG PRESS =====
    if (pressDuration >= LONG_PRESS_TIME) {
      tapCount = 0;  // cancel taps

      if (liveMode) {
        Serial.println("Cannot start video during live mode");
        return;
      }

      Serial.println("Long press");
      // handleLongPress();
      return;
    }

    // ===== SHORT TAP =====
    if (!isRecording) {
      tapCount++;
      lastTapTime = millis();
    }
  }

  // ---------- TAP EVALUATION ----------
  if (tapCount > 0 && (millis() - lastTapTime > DOUBLE_TAP_TIME)) {

    if (tapCount == 2) {
      Serial.println("Double tap");
      // handleTap();

    } else if (tapCount == 3) {
      if (isRecording && !liveMode) {
        Serial.println("Cannot start live mode during video recording");
      } else {
        Serial.println("Triple tap");
        handleTripleTap();
      }
    }

    tapCount = 0;
  }
}



void handleTap() {
  Serial.println("Double tap");
  sendCmd("DOUBLE_TAP");
}

void handleLongPress() {
  if (isRecording) {
    Serial.println("Stop recording");
    sendCmd("STOP_REC");
    isRecording = false;
    totalSent = 0;  // reset for next session
  } else {
    Serial.println("Start recording");
    sendCmd("START_REC");
    isRecording = true;
    totalSent = 0;  // start fresh
  }
}


// functions for mic
void setupI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
}



void showImage() {

  display.clearDisplay();

  display.drawBitmap(
    0,
    0,
    imageBuffer,
    128,
    64,
    WHITE
  );

  display.display();

  Serial.println("Image displayed on OLED");
}



void audioTask(void *pvParameters) {

  while(true) {
    if (!isRecording) {
      vTaskDelay(5);
      continue;
    }

    if (!liveMode && totalSent >= TOTAL_BYTES) {
      Serial.println("⏹️ Max audio length reached");
      sendCmd("STOP_REC");
      isRecording = false;
      totalSent = 0;
      continue;
    }
    size_t bytesRead;

    i2s_read(I2S_NUM_0, audioSamples, CHUNK_SIZE, &bytesRead, portMAX_DELAY);

    int samples = bytesRead / 2;

    // ---------- measure peak ----------
    int32_t peak = 0;
    for (int i = 0; i < samples; i++) {
      int32_t s = abs(audioSamples[i]);
      if (s > peak) peak = s;
    }

    // ---------- AGC ----------
    if (peak > 0) {
      float desiredGain = (float)TARGET_LEVEL / peak;
      currentGain = currentGain * 0.9 + desiredGain * 0.1;

      if (currentGain > MAX_GAIN) currentGain = MAX_GAIN;
      if (currentGain < MIN_GAIN) currentGain = MIN_GAIN;
    }

    // ---------- apply gain ----------
    for (int i = 0; i < samples; i++) {
      int32_t sample = audioSamples[i] * currentGain;

      if (sample > 32767) sample = 32767;
      if (sample < -32768) sample = -32768;

      audioSamples[i] = (int16_t)sample;
    }

    // ---------- send audio with sequence number ----------
    uint8_t packet[1 + 2 + CHUNK_SIZE];

    packet[0] = PKT_AUDIO;

    // add sequence number (little endian)
    packet[1] = audioPacketSeq & 0xFF;
    packet[2] = (audioPacketSeq >> 8) & 0xFF;

    memcpy(packet + 3, audioSamples, bytesRead);

    esp_now_send(camMAC, packet, bytesRead + 3);

    audioPacketSeq++;
    totalSent += bytesRead;

  }
}

void uartTask(void *pvParameters) {

  while(true) {

    while(Serial2.available()) {

      uint8_t b = Serial2.read();

      if(b == 0xAA) {
        receivingImage = true;
        imageIndex = 0;
        continue;
      }

      if(b == 0x55 && receivingImage) {
        receivingImage = false;
        newImageReady = true;
        continue;
      }

      if(receivingImage && imageIndex < 1024) {
        imageBuffer[imageIndex++] = b;
      }
    }

    vTaskDelay(1);
  }
}

void displayTask(void *pvParameters) {

  while(true) {

    if(newImageReady) {

      showImage();

      newImageReady = false;
    }

    vTaskDelay(10);
  }
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, RXD2, TXD2);

  pinMode(TOUCH_PIN, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);

  initEspNow();
  addPeer();
  esp_now_register_recv_cb(onReceive);

  setupI2S();



  Serial.println("Initializing I2C...");
  Wire.begin(21, 22);   // SDA, SCL

  Serial.println("Scanning for OLED...");

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("❌ OLED not found at 0x3C");
    while(true);
  }

  Serial.println("✅ OLED initialized successfully");

  display.clearDisplay();

  display.setTextSize(2);
  display.setTextColor(WHITE);
  display.setCursor(10,10);

  display.println("Hello");
  display.println("from esp32");

  display.display();

  Serial.println("Initial text displayed");

  xTaskCreatePinnedToCore(
    audioTask,
    "audioTask",
    4096,
    NULL,
    3,
    NULL,
    0
  );

  xTaskCreatePinnedToCore(
    uartTask,
    "uartTask",
    2048,
    NULL,
    2,
    NULL,
    1
  );

  xTaskCreatePinnedToCore(
    displayTask,
    "displayTask",
    2048,
    NULL,
    1,
    NULL,
    1
  );
}

// void loop() {
//   handleTouch();

//   // do nothing if not recording
//   if (!isRecording) {
//     delay(5);
//     return;
//   }
//   // stop automatically at 30s safety limit
//   // live conversation should NOT auto stop. so !liveMode
//   if (!liveMode && totalSent >= TOTAL_BYTES) {
//     Serial.println("⏹️ Max audio length reached");
//     sendCmd("STOP_REC");
//     isRecording = false;
//     totalSent = 0;
//     return;
//   }

// }

void loop() {
  handleTouch();
  delay(10);
}
