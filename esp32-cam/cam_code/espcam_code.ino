// Complete sketch: recorder + AsyncWebServer (no live stream)
// 11-02-26 12:45am integrated the two chips and the visual and audio is being recorded completely
// 14/02/26 now here to implement websocket architecture for the live conversation through audio mic
// implemented audio pipeline.
// 10/3/26 now implementing translation pipeline, getting the data from phone back(translated text/image) and
// sending to wroom through UART


#include "esp_camera.h"
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <stdio.h>
#include <string.h>
#include "FS.h"
#include "SD_MMC.h"
#include <time.h>


#include <esp_now.h> //for connecting two chips

#define UART_TX 1   // CAM TX
#define UART_RX 3   // CAM RX


String currentVideoDir = "";


// for differentiating between the audio and command
#define PKT_CONTROL 0x01
#define PKT_AUDIO   0x02



// for live conversation, establishing the web socket
AsyncWebSocket ws("/ws");

bool wsClientConnected = false;
bool liveConversation = false;     // true when triple tap streaming



// Select camera model
#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

// ================= TOUCH + LED =================
#define TOUCH_PIN 13
#define TOUCH_THRESHOLD 30
#define MAX_RECORD_TIME 30000     // 30 seconds max

#define LED_PIN 4                // ESP32-CAM flash LED

volatile bool sdBusy = false;  // for not just loading when requested for images when the video is being transfered.


volatile bool stopRequested = false;
volatile bool isRecording = false;

volatile bool recordingDone = false;
TaskHandle_t recordTaskHandle = NULL;  //parallel execution

bool touchPrev = false;
unsigned long touchStartTime = 0;


// for audio

File audioFile;
uint32_t audioBytes = 0;
bool audioRecording = false;

#define SAMPLE_RATE 16000

// ---------------- WAV HEADER ----------------
void writeWavHeader(File &file, uint32_t dataSize) {
  uint32_t fileSize = dataSize + 36;

  uint16_t audioFormat   = 1;     // PCM
  uint16_t numChannels   = 1;     // mono
  uint32_t sampleRate    = SAMPLE_RATE;
  uint32_t byteRate      = SAMPLE_RATE * 2;
  uint16_t blockAlign    = 2;
  uint16_t bitsPerSample = 16;

  file.seek(0);

  file.write((const uint8_t *)"RIFF", 4);
  file.write((uint8_t *)&fileSize, 4);
  file.write((const uint8_t *)"WAVE", 4);
  file.write((const uint8_t *)"fmt ", 4);

  uint32_t subChunk1Size = 16;
  file.write((uint8_t *)&subChunk1Size, 4);
  file.write((uint8_t *)&audioFormat, 2);
  file.write((uint8_t *)&numChannels, 2);
  file.write((uint8_t *)&sampleRate, 4);
  file.write((uint8_t *)&byteRate, 4);
  file.write((uint8_t *)&blockAlign, 2);
  file.write((uint8_t *)&bitsPerSample, 2);

  file.write((const uint8_t *)"data", 4);
  file.write((uint8_t *)&dataSize, 4);
}


// Video constraints
const int fps = 10;        // frames per second (target)
const int frame_interval = 1000 / fps;

// Global variables
File videoFile;
#define PATH_LEN 96
char tmpFileName[PATH_LEN];
char finalFileName[PATH_LEN];
int frameCount = 0;
unsigned long startTime;
uint32_t movi_size = 0;
uint32_t jpeg_size = 0;

// Index entry structure for AVI
struct avi_idx1_entry {
  uint32_t chunk_id;
  uint32_t flags;
  uint32_t offset;
  uint32_t size;
};

// We will store the index in a temporary file to save RAM
File indexFile;

// Async web server + mutex
AsyncWebServer asyncServer(80);
SemaphoreHandle_t sdMutex = NULL;

void recordVideoTask(void *pvParameters); // forward prototype


// Quick helper: recording-in-progress response  
void respondRecording(AsyncWebServerRequest *req) {
  req->send(503, "application/json", "{\"status\":\"recording_in_progress\",\"message\":\"Recording in progress. Try again later.\"}");
}

// for connecting to the other chip
// -------- Message structure --------
typedef struct {
  char cmd[16];
} ControlMsg;

// -------- Forward declaration --------
void sendStatus(const char* text, const uint8_t* mac);




// for ack
void ensurePeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

// ring buffer for ....
// ---------- AUDIO RING BUFFER (define, do not extern) ----------
#ifndef AUDIO_RING_SIZE
  #define AUDIO_RING_SIZE 16384   // 16KB, tune down to 8192 if memory tight
#endif

uint8_t audioRingBuffer[AUDIO_RING_SIZE];
volatile size_t audioWritePos = 0;
volatile size_t audioReadPos  = 0;
volatile size_t audioBufferedBytes = 0; // number of bytes currently in buffer


uint32_t activeClientId = 0;
uint8_t wroomMac[6] = {0};

// WebSocket Event Handler
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  // Access frame info to check if data is Text or Binary
  AwsFrameInfo *info = (AwsFrameInfo*)arg;

  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WS Client connected: %u\n", client->id());
      wsClientConnected = true;
      activeClientId = client->id();
      break;

    case WS_EVT_DISCONNECT:
      Serial.printf("WS Client disconnected: %u\n", client->id());
      if (ws.count() == 0) {
        wsClientConnected = false;
        activeClientId = 0;
      }
      
      if (liveConversation) {
        Serial.println("Phone disconnected → stopping live mode");
        liveConversation = false;
        audioRecording = false;
        sendStatus("LIVE_STOPPED", wroomMac); 
      }
      break;

    case WS_EVT_DATA:
      // 1. Handle TEXT messages (PING, etc.)
      if (info->opcode == WS_TEXT) {
        if (strncmp((char*)data, "PING", 4) == 0) {
          client->text("PONG");
          Serial.println("WS: Received PING, sent PONG");
        } 
        // Backward compatibility for text translations if still used
        else if (strncmp((char*)data, "TRANS:", 6) == 0) {
          char* translatedText = (char*)data + 6; 
          Serial.printf("WS Text Translation: %s\n", translatedText);
        }
      } 
      
      // 2. Handle BINARY messages (The 128x64 image bitmap)
      else if (info->opcode == WS_BINARY) {
        if (len == 1024) {
          Serial.println("WS: Received 1024 byte image");
          // Send start marker
          Serial2.write(0xAA);
          // Send bitmap
          Serial2.write(data, 1024);
          // Send end marker
          Serial2.write(0x55);
          Serial.println("Image sent to WROOM via UART");
        } else {
          Serial.printf("****WS: Received binary data of unexpected length: %u\n", len);
        }
      }
      break;

    case WS_EVT_PONG:
    case WS_EVT_ERROR:
      break;
  }
}




// -------- Receive callback --------
void onReceive(const esp_now_recv_info* info, const uint8_t* incomingData, int len) {

  if (len <= 1) return;

  const uint8_t packetType = incomingData[0];
  const uint8_t* payload  = incomingData + 1;
  int payloadLen = len - 1;

  const uint8_t* mac = info->src_addr;
  memcpy(wroomMac, info->src_addr, 6);
  ensurePeer(mac);

  // ================= CONTROL PACKETS ===================
  if (packetType == PKT_CONTROL) {

    ControlMsg msg;
    memcpy(&msg, payload, sizeof(msg));

    Serial.print("CMD: ");
    Serial.println(msg.cmd);

    // ---------- DOUBLE TAP (Photo) ----------
    if (strcmp(msg.cmd, "DOUBLE_TAP") == 0) {
      captureImage();
      sendStatus("PHOTO_OK", mac);
    }

    // ---------- VIDEO RECORD START ----------
    else if (strcmp(msg.cmd, "START_REC") == 0) {
      startVideo();
      audioRecording = true;   // enable audio ring buffer
      sendStatus("REC_STARTED", mac);
    }

    // ---------- VIDEO RECORD STOP ----------
    else if (strcmp(msg.cmd, "STOP_REC") == 0) {
      stopVideo();
      audioRecording = false;
      sendStatus("REC_STOPPED", mac);
    }

    // ---------- TRIPLE TAP (Live Conversation) ----------
    else if (strcmp(msg.cmd, "TRIPLE_TAP") == 0) {

      if (!wsClientConnected) {
        Serial.println("App not connected. Ignoring triple tap.");
        sendStatus("APP_NOT_READY", mac);
        return;
      }
      // // Phone dependency removed
      // Serial.println("Starting Live Conversation (Standalone Mode)");

      if (!liveConversation) {
        Serial.println("Starting Live Conversation");
        liveConversation = true;
        audioRecording = true;   // reuse ring buffer
        sendStatus("LIVE_STARTED", mac);
      }
      else {
        Serial.println("Stopping Live Conversation");
        liveConversation = false;
        audioRecording = false;
        sendStatus("LIVE_STOPPED", mac);
      }
    }
  }

  // ================= AUDIO PACKETS =====================
  else if (packetType == PKT_AUDIO) {

    if (!audioRecording) return;
    if (payloadLen < 2) return;

    uint16_t seq = payload[0] | (payload[1] << 8);

    static uint16_t lastSeq = 0xFFFF;

    if (seq == lastSeq) {
      return;  // duplicate
    }

    lastSeq = seq;

    const uint8_t* audioData = payload + 2;
    int audioLen = payloadLen - 2;

    // ================= VIDEO RECORD MODE =================
    if (!liveConversation) {

      for (int i = 0; i < audioLen; i++) {

        if (audioBufferedBytes >= AUDIO_RING_SIZE) {
          audioReadPos = (audioReadPos + 1) % AUDIO_RING_SIZE;
          audioBufferedBytes--;
        }

        audioRingBuffer[audioWritePos] = audioData[i];
        audioWritePos = (audioWritePos + 1) % AUDIO_RING_SIZE;
        audioBufferedBytes++;
      }
    }

    // ================= LIVE STREAM MODE =================
    else {
      // just buffer it, same as video mode
      for (int i = 0; i < audioLen; i++) {
          if (audioBufferedBytes >= AUDIO_RING_SIZE) {
              audioReadPos = (audioReadPos + 1) % AUDIO_RING_SIZE;
              audioBufferedBytes--;
          }
          audioRingBuffer[audioWritePos] = audioData[i];
          audioWritePos = (audioWritePos + 1) % AUDIO_RING_SIZE;
          audioBufferedBytes++;
      }
    }
  }
}





// -------- Send ACK/status --------
void sendStatus(const char* text, const uint8_t* mac) {
  ControlMsg msg;
  strcpy(msg.cmd, text);
  esp_now_send(mac, (uint8_t*)&msg, sizeof(msg));
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

String getTimeStamp() {
  static uint32_t bootId = millis();
  uint32_t t = millis();

  char buf[32];
  sprintf(buf, "%lu_%lu", bootId, t);
  return String(buf);
}


// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, UART_RX, UART_TX);
  WiFi.mode(WIFI_AP_STA); //for both ESP-NOW and AP
  delay(100);

  Serial.println("Starting...");
  // Start Access Point
  const char *ap_ssid = 'ESP_WIFI_Name';
  const char *ap_pass = 'ESP_WIFI_Password';

  WiFi.softAP(ap_ssid, ap_pass);

  // not go further till chips not connected..
  initEspNow();
  esp_now_register_recv_cb(onReceive);

  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  ws.onEvent(onWsEvent);
  asyncServer.addHandler(&ws);

  asyncServer.begin();

  

  // Camera config
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  // Init with high specs for better video if PSRAM found
  if(psramFound()){
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  // Camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return;
  }

  // SD card init in 1-bit mode (frees some GPIOs)
  if(!SD_MMC.begin("/sdcard", true)){
    Serial.println("SD Card Mount Failed");
    return;
  }

  uint8_t cardType = SD_MMC.cardType();
  if(cardType == CARD_NONE){
    Serial.println("No SD Card attached");
    return;
  }

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);   // LED OFF initially

  pinMode(TOUCH_PIN, INPUT_PULLUP); // touch pin

  // create mutex for SD access
  sdMutex = xSemaphoreCreateMutex();

  

  // Setup AsyncWebServer routes
  asyncServer.on("/images", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (isRecording) { respondRecording(req); return; }
    if (sdBusy) {
      req->send(503, "text/plain", "SD Busy");
      return;
    }
    if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);

    String payload = "[";
    File root = SD_MMC.open("/Images");
    bool first = true;

    if (root && root.isDirectory()) {
      File f = root.openNextFile();
      while (f) {
        String name = f.name();   // /Images/photo1.jpg

        if (!f.isDirectory() && (name.endsWith(".jpg") || name.endsWith(".jpeg") || name.endsWith(".png"))) {
          int slash = name.lastIndexOf('/');
          String cleanName = name.substring(slash + 1);

          if (!first) payload += ",";
          payload += "\"" + cleanName + "\"";
          first = false;
        }

        f.close();
        f = root.openNextFile();
      }
      root.close();
    }

    if (sdMutex) xSemaphoreGive(sdMutex);

    payload += "]";
    req->send(200, "application/json", payload);
  });

  asyncServer.on("/image", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (isRecording) { respondRecording(req); return; }
    if (sdBusy) {
      req->send(503, "text/plain", "SD Busy");
      return;
    }

    if (!req->hasParam("name")) {
      req->send(400, "text/plain", "Missing image name");
      return;
    }

    String fileName = req->getParam("name")->value();
    String path = "/Images/" + fileName;

    if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
    bool exists = SD_MMC.exists(path);
    if (sdMutex) xSemaphoreGive(sdMutex);

    if (!exists) {
      req->send(404, "text/plain", "Image not found");
      return;
    }

    AsyncWebServerResponse *response = req->beginResponse(SD_MMC, path, "image/jpeg");

    // Improve app performance (caching)
    response->addHeader("Cache-Control", "public, max-age=86400");

    // Uncomment if you want forced download instead of preview
    // response->addHeader("Content-Disposition", "attachment; filename=" + fileName);

    req->send(response);
  });


  
  // /videos -> JSON array of available video folders
  asyncServer.on("/videos", HTTP_GET, [](AsyncWebServerRequest *req){
    if (isRecording) { respondRecording(req); return; }

    if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);

    String payload = "[";
    File root = SD_MMC.open("/Videos");
    File f = root.openNextFile();
    bool first = true;

    while (f) {
      if (f.isDirectory()) {
        String folderName = String(f.name());   // e.g. /Videos/Video_BOOT_xxx
        int lastSlash = folderName.lastIndexOf('/');
        if (lastSlash >= 0)
          folderName = folderName.substring(lastSlash + 1);

        if (!first) payload += ",";
        payload += "\"" + folderName + "\"";
        first = false;
      }
      f.close();
      f = root.openNextFile();
    }

    root.close();
    if (sdMutex) xSemaphoreGive(sdMutex);

    payload += "]";
    req->send(200, "application/json", payload);
  });


  // /download?folder=Video_BOOT_xxx&type=video
  // /download?folder=Video_BOOT_xxx&type=audio
  asyncServer.on("/download", HTTP_GET, [](AsyncWebServerRequest *req){

    if (isRecording) {
      req->send(503, "application/json",
        "{\"status\":\"recording_in_progress\"}");
      return;
    }

    if (!req->hasParam("folder") || !req->hasParam("type")) {
      req->send(400, "text/plain", "Missing params");
      return;
    }

    String folder = req->getParam("folder")->value();
    String type   = req->getParam("type")->value();

    while (folder.startsWith("/"))
      folder = folder.substring(1);

    String fileName;

    if (type == "video")
      fileName = "video.avi";
    else if (type == "audio")
      fileName = "audio.wav";
    else {
      req->send(400, "text/plain", "Invalid type");
      return;
    }

    String fullPath = "/Videos/" + folder + "/" + fileName;

    if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
    bool exists = SD_MMC.exists(fullPath.c_str());
    if (sdMutex) xSemaphoreGive(sdMutex);

    if (!exists) {
      req->send(404, "text/plain", "File not found");
      return;
    }

    sdBusy = true;

    req->onDisconnect([]() {
      sdBusy = false;
      Serial.println("Transfer finished, SD free");
    });

    String mime = (type == "video") ? "video/x-msvideo" : "audio/wav";

    AsyncWebServerResponse *response =
      req->beginResponse(SD_MMC, fullPath, mime);

    response->addHeader("Cache-Control", "no-cache");
    req->send(response);

    Serial.println("Sent: " + fullPath);
  });


  // Not found
  asyncServer.onNotFound([](AsyncWebServerRequest *req){
    req->send(404, "text/plain", "Not found");
  });

  asyncServer.begin();
  Serial.println("HTTP server started (AP mode)");
  Serial.println("Everything good to go !!! Ayazzz..");
}


void flashTwice() {
  for (int i = 0; i < 2; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(50);
    digitalWrite(LED_PIN, LOW);
    delay(50);
  }
}

void captureImage() {

  // Phone-style flash
  flashTwice();

  // Small delay before capture (camera exposure settles)
  delay(40);


  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    return;
  }

  String path = "/Images/img_" + String(millis()) + ".jpg";
  File file = SD_MMC.open(path, FILE_WRITE);
  file.write(fb->buf, fb->len);
  file.close();

  esp_camera_fb_return(fb);
  Serial.println("Image saved: " + path);
}

// ---------- Recording control ----------
void startVideo() {

  // Create timestamped directory
  String ts = getTimeStamp();
  currentVideoDir = "/Videos/Video_" + ts;

  if (!SD_MMC.exists(currentVideoDir)) {
    if (!SD_MMC.mkdir(currentVideoDir)) {
      Serial.println("❌ Failed to create video directory");
      return;
    }
  }

  Serial.println("📁 Video dir: " + currentVideoDir);

  // Prepare file paths
  snprintf(tmpFileName, sizeof(tmpFileName),
           "%s/video.tmp", currentVideoDir.c_str());

  snprintf(finalFileName, sizeof(finalFileName),
           "%s/video.avi", currentVideoDir.c_str());

  if (SD_MMC.exists(tmpFileName) || SD_MMC.exists(finalFileName)) {
    Serial.println("❌ Video file already exists");
    return;
  }

  Serial.printf("🎥 Recording to (tmp): %s\n", tmpFileName);

  // Open temp video file ONLY
  videoFile = SD_MMC.open(tmpFileName, FILE_WRITE);
  if (!videoFile) {
    Serial.println("❌ Failed to open temp video file");
    return;
  }

  // Reset audio ring buffer state
  audioWritePos = 0;
  audioReadPos  = 0;
  audioBufferedBytes = 0;
  audioBytes = 0;

  audioRecording = true;   // allow ESP-NOW audio packets

  // Recording state
  isRecording = true;
  recordingDone = false;

  // Start recording task (single SD writer)
  xTaskCreatePinnedToCore(
    recordVideoTask,
    "recTask",
    4096 * 4,
    NULL,
    2,
    &recordTaskHandle,
    1
  );

  Serial.println("✅ Video + audio capture started");
}



// The recording task: capture loop and finalization (only this task writes final AVI header)
void recordVideoTask(void *pvParameters) {

  digitalWrite(LED_PIN, HIGH);

  /* ================== OPEN INDEX FILE ================== */
  if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
  indexFile = SD_MMC.open("/Videos/idx.tmp", FILE_WRITE);
  if (sdMutex) xSemaphoreGive(sdMutex);

  if (!indexFile) {
    Serial.println("❌ Failed to open idx.tmp");
    videoFile.close();
    isRecording = false;
    recordingDone = true;
    vTaskDelete(NULL);
    return;
  }

  /* ================== OPEN AUDIO FILE ================== */
  String audioPath = currentVideoDir + "/audio.wav";
  if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
  audioFile = SD_MMC.open(audioPath, FILE_WRITE);
  if (audioFile) {
    writeWavHeader(audioFile, 0);  // placeholder
    Serial.println("🎧 Audio file opened");
  } else {
    Serial.println("❌ Failed to open audio.wav");
  }
  if (sdMutex) xSemaphoreGive(sdMutex);

  /* ================== AVI HEADER PLACEHOLDER ================== */
  uint8_t headerBuf[250];
  memset(headerBuf, 0, sizeof(headerBuf));
  if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
  videoFile.write(headerBuf, sizeof(headerBuf));
  videoFile.flush();
  if (sdMutex) xSemaphoreGive(sdMutex);

  movi_size = 0;
  frameCount = 0;
  startTime = millis();
  unsigned long lastFrameTimeLocal = 0;

  Serial.println("🎥 Recording started");

  /* ================== MAIN RECORD LOOP ================== */
  while ((millis() - startTime < MAX_RECORD_TIME) && isRecording) {

    /* ---------- VIDEO FRAME ---------- */
    if (millis() - lastFrameTimeLocal >= frame_interval) {
      lastFrameTimeLocal = millis();

      camera_fb_t *fb = esp_camera_fb_get();
      if (!fb) continue;

      uint32_t dc_id = 0x63643030; // '00dc'
      uint32_t chunk_size = fb->len;
      uint32_t padding = (4 - (chunk_size % 4)) % 4;
      uint32_t total_chunk_size = chunk_size + padding;

      // write video chunk (take mutex only for the actual writes)
      if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
      videoFile.write((uint8_t*)&dc_id, 4);
      videoFile.write((uint8_t*)&total_chunk_size, 4);
      videoFile.write(fb->buf, fb->len);
      if (padding) {
        uint8_t zero = 0;
        for (int i = 0; i < padding; i++) videoFile.write(&zero, 1);
      }
      if (sdMutex) xSemaphoreGive(sdMutex);

      // write index entry (also protected)
      struct avi_idx1_entry entry;
      entry.chunk_id = dc_id;
      entry.flags = 0x10;
      entry.offset = movi_size + 4;
      entry.size = chunk_size;
      if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
      indexFile.write((uint8_t*)&entry, sizeof(entry));
      if (sdMutex) xSemaphoreGive(sdMutex);

      movi_size += (8 + total_chunk_size);
      frameCount++;

      esp_camera_fb_return(fb);
    }

    /* ---------- AUDIO DRAIN (wrap-safe) ---------- */
    if (audioFile && audioBufferedBytes > 0) {

      // write up to N bytes per iteration to avoid starving video
      size_t want = min((size_t)256, (size_t)audioBufferedBytes);

      // write first contiguous chunk
      size_t firstChunk = min(want, (size_t)(AUDIO_RING_SIZE - audioReadPos));
      if (firstChunk > 0) {
        if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
        audioFile.write(&audioRingBuffer[audioReadPos], firstChunk);
        if (sdMutex) xSemaphoreGive(sdMutex);

        audioReadPos = (audioReadPos + firstChunk) % AUDIO_RING_SIZE;
        audioBufferedBytes -= firstChunk;
        audioBytes += firstChunk;
        want -= firstChunk;
      }

      // write wrapped remainder
      if (want > 0) {
        if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
        audioFile.write(&audioRingBuffer[audioReadPos], want);
        if (sdMutex) xSemaphoreGive(sdMutex);

        audioReadPos = (audioReadPos + want) % AUDIO_RING_SIZE;
        audioBufferedBytes -= want;
        audioBytes += want;
      }
    }

    vTaskDelay(1 / portTICK_PERIOD_MS);
  }

  /* ================== FINALIZE ================== */
  digitalWrite(LED_PIN, LOW);
  audioRecording = false;

  /* ---- flush remaining audio (wrap-safe) ---- */
  while (audioBufferedBytes > 0 && audioFile) {
    size_t want = min((size_t)256, (size_t)audioBufferedBytes);
    size_t firstChunk = min(want, (size_t)(AUDIO_RING_SIZE - audioReadPos));
    if (firstChunk > 0) {
      if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
      audioFile.write(&audioRingBuffer[audioReadPos], firstChunk);
      if (sdMutex) xSemaphoreGive(sdMutex);

      audioReadPos = (audioReadPos + firstChunk) % AUDIO_RING_SIZE;
      audioBufferedBytes -= firstChunk;
      audioBytes += firstChunk;
      want -= firstChunk;
    }
    if (want > 0) {
      if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
      audioFile.write(&audioRingBuffer[audioReadPos], want);
      if (sdMutex) xSemaphoreGive(sdMutex);

      audioReadPos = (audioReadPos + want) % AUDIO_RING_SIZE;
      audioBufferedBytes -= want;
      audioBytes += want;
    }
  }

  if (audioFile) {
    if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
    writeWavHeader(audioFile, audioBytes);
    audioFile.close();
    if (sdMutex) xSemaphoreGive(sdMutex);
    Serial.println("🎧 Audio finalized");
  }

  if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
  indexFile.close();
  if (sdMutex) xSemaphoreGive(sdMutex);

  /* ================== WRITE idx1 ================== */
  if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
  indexFile = SD_MMC.open("/Videos/idx.tmp", FILE_READ);
  if (indexFile) {
    uint32_t idx1_id = 0x31786469;
    uint32_t idx1_size = frameCount * sizeof(struct avi_idx1_entry);
    videoFile.write((uint8_t*)&idx1_id, 4);
    videoFile.write((uint8_t*)&idx1_size, 4);

    uint8_t buf[512];
    while (indexFile.available()) {
      int r = indexFile.read(buf, sizeof(buf));
      if (r > 0) videoFile.write(buf, r);
    }
    indexFile.close();
    SD_MMC.remove("/Videos/idx.tmp");
  } else {
    Serial.println("Warning: idx.tmp not found during finalization");
  }
  if (sdMutex) xSemaphoreGive(sdMutex);

  /* ================== AVI HEADER FIXUP ================== */
  if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
  uint32_t total_size = videoFile.size();
  videoFile.seek(0, SeekSet);

  uint32_t riff_size = total_size - 8;
  videoFile.write((uint8_t*)"RIFF", 4);
  videoFile.write((uint8_t*)&riff_size, 4);
  videoFile.write((uint8_t*)"AVI ", 4);

  // LIST hdrl
  videoFile.write((uint8_t*)"LIST", 4);
  uint32_t hdrl_size = 172; // Approximate
  videoFile.write((uint8_t*)&hdrl_size, 4);
  videoFile.write((uint8_t*)"hdrl", 4);
  // avih
  videoFile.write((uint8_t*)"avih", 4);
  uint32_t avih_size = 56;
  videoFile.write((uint8_t*)&avih_size, 4);

  uint32_t us_per_frame = 1000000 / fps;
  videoFile.write((uint8_t*)&us_per_frame, 4);
  uint32_t max_bytes = 0; videoFile.write((uint8_t*)&max_bytes, 4);
  uint32_t paddingLocal = 0; videoFile.write((uint8_t*)&paddingLocal, 4);
  uint32_t flags = 0x10; videoFile.write((uint8_t*)&flags, 4); // HASINDEX
  videoFile.write((uint8_t*)&frameCount, 4);
  uint32_t initial_frames = 0; videoFile.write((uint8_t*)&initial_frames, 4);
  uint32_t streams = 1; videoFile.write((uint8_t*)&streams, 4);
  uint32_t buf_size = 102400; videoFile.write((uint8_t*)&buf_size, 4);
  uint32_t width = 640; videoFile.write((uint8_t*)&width, 4);
  uint32_t height = 480; videoFile.write((uint8_t*)&height, 4);
  uint32_t reserved[4] = {0,0,0,0}; videoFile.write((uint8_t*)reserved, 16);

  // LIST strl + strh + strf (same as before)
  videoFile.write((uint8_t*)"LIST", 4);
  uint32_t strl_size = 108;
  videoFile.write((uint8_t*)&strl_size, 4);
  videoFile.write((uint8_t*)"strl", 4);
  // strh
  videoFile.write((uint8_t*)"strh", 4);
  uint32_t strh_size = 56;
  videoFile.write((uint8_t*)&strh_size, 4);
  videoFile.write((uint8_t*)"vids", 4);
  videoFile.write((uint8_t*)"MJPG", 4);
  videoFile.write((uint8_t*)&paddingLocal, 4); // flags
  videoFile.write((uint8_t*)&paddingLocal, 4); // priority
  videoFile.write((uint8_t*)&paddingLocal, 4); // initial frames
  uint32_t scale = 1; videoFile.write((uint8_t*)&scale, 4);
  videoFile.write((uint8_t*)&fps, 4);
  uint32_t start = 0; videoFile.write((uint8_t*)&start, 4);
  videoFile.write((uint8_t*)&frameCount, 4);
  videoFile.write((uint8_t*)&buf_size, 4);
  int32_t quality = -1; videoFile.write((uint8_t*)&quality, 4);
  videoFile.write((uint8_t*)&paddingLocal, 4); // sample size
  uint32_t rc_frame[2] = {0, 0}; videoFile.write((uint8_t*)rc_frame, 8);

  // strf
  videoFile.write((uint8_t*)"strf", 4);
  uint32_t strf_size = 40;
  videoFile.write((uint8_t*)&strf_size, 4);
  uint32_t bi_size = 40; videoFile.write((uint8_t*)&bi_size, 4);
  videoFile.write((uint8_t*)&width, 4);
  videoFile.write((uint8_t*)&height, 4);
  uint16_t planes = 1; videoFile.write((uint8_t*)&planes, 2);
  uint16_t bit_count = 24; videoFile.write((uint8_t*)&bit_count, 2);
  videoFile.write((uint8_t*)"MJPG", 4);
  uint32_t img_size = width * height * 3; videoFile.write((uint8_t*)&img_size, 4);
  videoFile.write((uint8_t*)&paddingLocal, 4); // xpels
  videoFile.write((uint8_t*)&paddingLocal, 4); // ypels
  videoFile.write((uint8_t*)&paddingLocal, 4); // colors used
  videoFile.write((uint8_t*)&paddingLocal, 4); // colors important

  // movi list header - write at the saved position (this mirrors your earlier logic)
  videoFile.seek(224, SeekSet); // Exact position depends on previous writes
  videoFile.write((uint8_t*)"LIST", 4);
  uint32_t movi_list_size = movi_size + 4;
  videoFile.write((uint8_t*)&movi_list_size, 4);
  videoFile.write((uint8_t*)"movi", 4);

  videoFile.flush();
  videoFile.close();
  if (sdMutex) xSemaphoreGive(sdMutex);

  /* ================== RENAME FILE ================== */
  if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
  if (SD_MMC.exists(tmpFileName)) {
    SD_MMC.rename(tmpFileName, finalFileName);
  }
  if (sdMutex) xSemaphoreGive(sdMutex);

  Serial.printf("✅ Video saved: %s | Frames: %d\n", finalFileName, frameCount);

  recordingDone = true;
  recordTaskHandle = NULL;
  vTaskDelete(NULL);
}



void stopVideo() {
  if (!isRecording) return;
  isRecording = false;

  unsigned long waitStart = millis();
  const unsigned long WAIT_TIMEOUT = 7000;
  while (!recordingDone && (millis() - waitStart) < WAIT_TIMEOUT) {
    delay(10);
  }

  if (!recordingDone) {
    Serial.println("Warning: recording task didn't finish within timeout");
  } else {
    Serial.println("Recording finalized");
  }

  // Do not write WAV header here — recordVideoTask() finalizes and closes audioFile.
  recordingDone = false; // ready for next recording
}



portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

void loop() {

  ws.cleanupClients();

  static unsigned long lastSend = 0;
  const unsigned long CHUNK_INTERVAL_MS = 16;   // 512 bytes @ 16kHz

  if (!liveConversation || !wsClientConnected)
      return;

  // pacing: send 1 chunk every 16ms
  if (millis() - lastSend < CHUNK_INTERVAL_MS)
      return;

  if (audioBufferedBytes < 512)
      return;

  AsyncWebSocketClient *client = ws.client(activeClientId);
  if (!client || !client->canSend())
      return;   // don't consume buffer if socket busy

  uint8_t chunk[512];

  // critical section to avoid race condition
  portENTER_CRITICAL(&mux);

  for (int i = 0; i < 512; i++) {
      chunk[i] = audioRingBuffer[audioReadPos];
      audioReadPos = (audioReadPos + 1) % AUDIO_RING_SIZE;
      audioBufferedBytes--;
  }

  portEXIT_CRITICAL(&mux);

  client->binary(chunk, 512);

  lastSend = millis();
}