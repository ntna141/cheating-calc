#include <WiFi.h>
#include "esp_camera.h"
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "mbedtls/base64.h"

// WiFi credentials
const char* ssid = "Abc";
const char* password = "11111111";

// OpenAI API Details
const char* openai_api_key = "";
const char* openai_api_url = "";

// Configurable prompts
const char* transcription_prompt = 
  "Im digitalizing my question bank, transcribe the question right ABOVE the pen, the pen is a marker for which question we are targeting "
  "send back Latex if necessary ";

const char* answer_prompt = 
  "Please provide a single letter wrapped in <answer></answer> tag for the multiple choice question "
  "";

// Camera pin definitions (AI-Thinker)
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

WebServer server(80);

// Store last image and results
uint8_t* lastImageBuffer = nullptr;
size_t lastImageSize = 0;
String lastTranscription = "";
String lastAnswer = "";
String lastExtractedAnswer = "";

// Camera config
camera_config_t camera_config;
bool camera_initialized = false;

// --- Utility Functions ---

String base64Encode(uint8_t* data, size_t len) {
  size_t output_len;
  mbedtls_base64_encode(NULL, 0, &output_len, data, len);

  char* base64_buf = (char*)malloc(output_len + 1);
  if (!base64_buf) {
    Serial.println("Failed to allocate memory for base64 buffer");
    return "";
  }

  if (mbedtls_base64_encode((unsigned char*)base64_buf, output_len,
                            &output_len, data, len) != 0) {
    Serial.println("Base64 encoding failed");
    free(base64_buf);
    return "";
  }
  base64_buf[output_len] = '\0';

  String base64_string = String(base64_buf);
  free(base64_buf);
  return base64_string;
}

String extractAnswerFromTags(const String& text) {
  int startTag = text.indexOf("<answer>");
  int endTag = text.indexOf("</answer>");
  
  if (startTag != -1 && endTag != -1 && endTag > startTag) {
    return text.substring(startTag + 8, endTag);
  }
  
  return text;
}

// --- API Functions ---

bool sendHttpRequest(const String& requestBody, String& response) {
  const int MAX_RETRIES = 3;
  const int RETRY_DELAY = 5000;
  
  for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
    Serial.printf("HTTP attempt %d/%d\n", attempt, MAX_RETRIES);
    
    HTTPClient http;
    http.begin(openai_api_url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + String(openai_api_key));
    http.setTimeout(60000); // Increased timeout to 60 seconds
    
    Serial.printf("Sending %d bytes...\n", requestBody.length());
    
    // Debug: Print first 200 chars of request
    Serial.println("Request preview:");
    Serial.println(requestBody.substring(0, 200) + "...");
    
    unsigned long startTime = millis();
    int httpResponseCode = http.POST(requestBody);
    unsigned long duration = millis() - startTime;
    
    Serial.printf("Request took %lu ms\n", duration);
    
    if (httpResponseCode > 0) {
      Serial.printf("HTTP Response code: %d\n", httpResponseCode);
      
      if (httpResponseCode == 200) {
        response = http.getString();
        http.end();
        Serial.println("Request successful!");
        return true;
      } else {
        String errorResponse = http.getString();
        Serial.printf("HTTP Error %d: %s\n", httpResponseCode, errorResponse.c_str());
      }
    } else {
      Serial.printf("HTTP failed, code: %d, error: %s\n", 
                    httpResponseCode, http.errorToString(httpResponseCode).c_str());
    }
    
    http.end();
    
    if (attempt < MAX_RETRIES) {
      Serial.printf("Retrying in %d seconds...\n", RETRY_DELAY / 1000);
      delay(RETRY_DELAY);
    }
  }
  
  Serial.println("All retry attempts failed");
  return false;
}

bool sendTextRequest(const String& prompt, String& response) {
  DynamicJsonDocument doc(2048);
  
  doc["model"] = "gpt-4o"; // Fixed: Use gpt-4o instead of gpt-4.1
  doc["max_tokens"] = 1000;
  doc["temperature"] = 0.7;
  
  JsonArray messages = doc.createNestedArray("messages");
  JsonObject userMessage = messages.createNestedObject();
  userMessage["role"] = "user";
  userMessage["content"] = prompt;
  
  String requestBody;
  serializeJson(doc, requestBody);
  
  Serial.println("Text request JSON:");
  Serial.println(requestBody);
  
  return sendHttpRequest(requestBody, response);
}

bool sendImageRequest(const String& base64Image, const String& prompt, String& response) {
  // Validate base64 image
  if (base64Image.length() == 0) {
    Serial.println("Base64 image is empty!");
    return false;
  }
  
  // Print base64 preview for debugging
  Serial.printf("Base64 preview (first 100 chars): %s...\n", base64Image.substring(0, 100).c_str());
  Serial.printf("Base64 length: %d characters\n", base64Image.length());
  
  const size_t jsonCapacity = base64Image.length() + 4096; // Increased buffer
  DynamicJsonDocument doc(jsonCapacity);

  doc["model"] = "gpt-4o";
  doc["max_tokens"] = 800;

  JsonArray messages = doc.createNestedArray("messages");
  JsonObject userMessage = messages.createNestedObject();
  userMessage["role"] = "user";

  JsonArray content = userMessage.createNestedArray("content");
  
  // Text part
  JsonObject textPart = content.createNestedObject();
  textPart["type"] = "text";
  textPart["text"] = prompt;

  // Image part
  JsonObject imagePart = content.createNestedObject();
  imagePart["type"] = "image_url";
  JsonObject imageUrl = imagePart.createNestedObject("image_url");
  imageUrl["url"] = "data:image/jpeg;base64," + base64Image;

  String requestBody;
  serializeJson(doc, requestBody);

  if (doc.overflowed()) {
    Serial.printf("JSON overflowed! Capacity: %zu, Usage: %zu\n", 
                  jsonCapacity, doc.memoryUsage());
    return false;
  }

  Serial.printf("JSON capacity: %zu, usage: %zu\n", jsonCapacity, doc.memoryUsage());
  
  return sendHttpRequest(requestBody, response);
}

// --- Camera Functions ---

bool initCamera() {
  camera_config.ledc_channel = LEDC_CHANNEL_0;
  camera_config.ledc_timer = LEDC_TIMER_0;
  camera_config.pin_d0 = Y2_GPIO_NUM;
  camera_config.pin_d1 = Y3_GPIO_NUM;
  camera_config.pin_d2 = Y4_GPIO_NUM;
  camera_config.pin_d3 = Y5_GPIO_NUM;
  camera_config.pin_d4 = Y6_GPIO_NUM;
  camera_config.pin_d5 = Y7_GPIO_NUM;
  camera_config.pin_d6 = Y8_GPIO_NUM;
  camera_config.pin_d7 = Y9_GPIO_NUM;
  camera_config.pin_xclk = XCLK_GPIO_NUM;
  camera_config.pin_pclk = PCLK_GPIO_NUM;
  camera_config.pin_vsync = VSYNC_GPIO_NUM;
  camera_config.pin_href = HREF_GPIO_NUM;
  camera_config.pin_sccb_sda = SIOD_GPIO_NUM;
  camera_config.pin_sccb_scl = SIOC_GPIO_NUM;
  camera_config.pin_pwdn = PWDN_GPIO_NUM;
  camera_config.pin_reset = RESET_GPIO_NUM;
  camera_config.xclk_freq_hz = 20000000;
  
  camera_config.pixel_format = PIXFORMAT_JPEG; // Ensure JPEG format
  
  // Reduced image size for better reliability
  if (psramFound()) {
    Serial.println("PSRAM found - using VGA resolution");
    camera_config.frame_size = FRAMESIZE_VGA; // 640x480 - reduced from SVGA
    camera_config.jpeg_quality = 12; // Lower quality for smaller file
    camera_config.fb_count = 2;
  } else {
    Serial.println("No PSRAM - using QVGA resolution");
    camera_config.frame_size = FRAMESIZE_QVGA; // 320x240
    camera_config.jpeg_quality = 15;
    camera_config.fb_count = 1;
  }
  
  camera_config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  Serial.println("Initializing camera...");
  esp_err_t err = esp_camera_init(&camera_config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }
  
  // Verify camera sensor
  sensor_t * s = esp_camera_sensor_get();
  if (s == NULL) {
    Serial.println("Camera sensor not found!");
    return false;
  }
  
  Serial.println("Camera initialized successfully.");
  s->set_vflip(s, 0);
  s->set_hmirror(s, 0);
  
  camera_initialized = true;
  return true;
}

void deinitCamera() {
  if (camera_initialized) {
    esp_camera_deinit();
    camera_initialized = false;
    Serial.println("Camera deinitialized.");
  }
}

void storeLastImage(uint8_t* imageData, size_t imageSize) {
  if (lastImageBuffer) free(lastImageBuffer);
  lastImageBuffer = (uint8_t*)malloc(imageSize);
  if (lastImageBuffer) {
    memcpy(lastImageBuffer, imageData, imageSize);
    lastImageSize = imageSize;
    Serial.printf("Stored image: %zu bytes\n", imageSize);
  } else {
    lastImageSize = 0;
    Serial.println("Failed to allocate memory for image storage");
  }
}

// --- Web Handlers ---

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32 OCR Camera</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial; background: #f0f0f0; margin: 0; padding: 0; }
    .container { max-width: 800px; margin: 20px auto; background: #fff; padding: 20px; border-radius: 10px; }
    img { max-width: 100%; border: 2px solid #ccc; border-radius: 5px; }
    button { padding: 12px 24px; font-size: 16px; margin: 10px 5px; background: #4CAF50; color: white; border: none; border-radius: 5px; cursor: pointer; }
    button:hover { background: #45a049; }
    .result-box { background: #f9f9f9; padding: 15px; margin: 15px 0; border-radius: 5px; border-left: 4px solid #4CAF50; }
    .answer-box { background: #e8f5e8; padding: 15px; margin: 15px 0; border-radius: 5px; border-left: 4px solid #2196F3; }
    .debug { background: #eee; padding: 10px; border-radius: 5px; font-size: 12px; margin-top: 20px; }
    pre { white-space: pre-wrap; word-wrap: break-word; }
  </style>
</head>
<body>
  <div class="container">
    <h2>🤖 ESP32 OCR Camera System</h2>
    
    <div>
      <form action="/capture" method="get" style="display: inline;">
        <button type="submit">📸 Capture & Process Question</button>
      </form>
      <button onclick="location.reload()">🔄 Refresh</button>
    </div>
    
    <h3>📷 Last Captured Image:</h3>
    )rawliteral";
    
  if (lastImageSize > 0) {
    html += "<img src=\"/image\" alt=\"Captured image\"><br>";
  } else {
    html += "<p>No image captured yet.</p>";
  }
  
  html += R"rawliteral(
    <div class="result-box">
      <h3>📝 Transcribed Question:</h3>
      <pre>)rawliteral" + lastTranscription + R"rawliteral(</pre>
    </div>
    
    <div class="result-box">
      <h3>💬 Full AI Response:</h3>
      <pre>)rawliteral" + lastAnswer + R"rawliteral(</pre>
    </div>
    
    <div class="answer-box">
      <h3>✅ Extracted Answer:</h3>
      <pre>)rawliteral" + lastExtractedAnswer + R"rawliteral(</pre>
    </div>
    
    <div class="debug">
      <b>🔧 Debug Info:</b><br>
      Camera initialized: )rawliteral" + String(camera_initialized ? "Yes" : "No") + R"rawliteral(<br>
      Last image size: )rawliteral" + String(lastImageSize) + R"rawliteral( bytes<br>
      Free heap: )rawliteral" + String(ESP.getFreeHeap()) + R"rawliteral( bytes<br>
      WiFi RSSI: )rawliteral" + String(WiFi.RSSI()) + R"rawliteral( dBm<br>
      PSRAM: )rawliteral" + String(psramFound() ? "Available" : "Not found") + R"rawliteral(<br>
    </div>
  </div>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleImage() {
  if (lastImageBuffer && lastImageSize > 0) {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.send_P(200, "image/jpeg", (const char*)lastImageBuffer, lastImageSize);
    Serial.println("Served image to client.");
  } else {
    server.send(404, "text/plain", "No image available");
    Serial.println("No image to serve.");
  }
}

void handleCapture() {
  server.sendHeader("Location", "/");
  server.send(303);
  Serial.println("Capture requested via web.");
  captureAndProcessQuestion();
}

// --- Serial Command Handler ---

void handleSerialCommands() {
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    command.toLowerCase();
    if (command == "take") {
      Serial.println("Serial: Taking picture and processing...");
      captureAndProcessQuestion();
    } else if (command == "status") {
      Serial.printf("Camera initialized: %s\n", camera_initialized ? "Yes" : "No");
      Serial.printf("Last image size: %zu bytes\n", lastImageSize);
      Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
      Serial.printf("WiFi RSSI: %d dBm\n", WiFi.RSSI());
      Serial.printf("PSRAM: %s\n", psramFound() ? "Available" : "Not found");
    } else if (command == "help") {
      Serial.println("Commands: take, status, help");
    } else if (command != "") {
      Serial.println("Unknown command. Type 'help'.");
    }
  }
}

// --- Main Processing Function ---

void captureAndProcessQuestion() {
  Serial.println("\n=== Starting Question Processing Cycle ===");
  
  lastTranscription = "Processing...";
  lastAnswer = "Processing...";
  lastExtractedAnswer = "Processing...";
  
  // Check WiFi
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected. Attempting reconnection...");
    WiFi.reconnect();
    delay(5000);
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi reconnection failed. Skipping cycle.");
      lastTranscription = "WiFi connection failed";
      lastAnswer = "WiFi connection failed";
      lastExtractedAnswer = "WiFi connection failed";
      return;
    }
  }
  
  Serial.printf("WiFi RSSI: %d dBm\n", WiFi.RSSI());
  Serial.printf("Free heap before capture: %d bytes\n", ESP.getFreeHeap());

  // Initialize camera
  Serial.println("Initializing camera...");
  if (!initCamera()) {
    Serial.println("Camera init failed.");
    lastTranscription = "Camera initialization failed";
    lastAnswer = "Camera initialization failed";
    lastExtractedAnswer = "Camera initialization failed";
    return;
  }
  
  // Capture image
  Serial.println("Capturing image...");
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed.");
    deinitCamera();
    lastTranscription = "Camera capture failed";
    lastAnswer = "Camera capture failed";
    lastExtractedAnswer = "Camera capture failed";
    return;
  }
  
  // Validate image format
  if (fb->format != PIXFORMAT_JPEG) {
    Serial.printf("Invalid image format: %d (expected JPEG: %d)\n", fb->format, PIXFORMAT_JPEG);
    esp_camera_fb_return(fb);
    deinitCamera();
    lastTranscription = "Invalid image format - not JPEG";
    lastAnswer = "Invalid image format - not JPEG";
    lastExtractedAnswer = "Invalid image format - not JPEG";
    return;
  }
  
  Serial.printf("Captured JPEG image: %dx%d, %zu bytes, format: %d\n", 
                fb->width, fb->height, fb->len, fb->format);
  
  // Store image for web display
  storeLastImage(fb->buf, fb->len);
  
  // Encode to base64
  Serial.println("Encoding to Base64...");
  String base64Image = base64Encode(fb->buf, fb->len);
  esp_camera_fb_return(fb);
  deinitCamera();
  
  if (base64Image.length() == 0) {
    Serial.println("Base64 encoding failed");
    lastTranscription = "Base64 encoding failed";
    lastAnswer = "Base64 encoding failed";
    lastExtractedAnswer = "Base64 encoding failed";
    return;
  }
  
  Serial.printf("Base64 encoding successful: %d characters\n", base64Image.length());
  Serial.printf("Free heap after encoding: %d bytes\n", ESP.getFreeHeap());

  // STEP 1: Transcribe the question
  Serial.println("\n--- STEP 1: Transcribing Question ---");
  String transcriptionResponse;
  bool transcriptionSuccess = sendImageRequest(base64Image, transcription_prompt, transcriptionResponse);

  if (!transcriptionSuccess) {
    Serial.println("Failed to transcribe question");
    lastTranscription = "Failed to transcribe question - API error";
    lastAnswer = "Cannot answer - transcription failed";
    lastExtractedAnswer = "Cannot answer - transcription failed";
    return;
  }

  // Parse transcription response
  DynamicJsonDocument transcriptionDoc(8192); // Increased size
  DeserializationError error = deserializeJson(transcriptionDoc, transcriptionResponse);

  if (error) {
    Serial.printf("Transcription JSON parse error: %s\n", error.c_str());
    Serial.println("Raw response (first 500 chars):");
    Serial.println(transcriptionResponse.substring(0, 500));
    lastTranscription = "JSON parse error: " + String(error.c_str());
    lastAnswer = "Cannot answer - JSON parse error";
    lastExtractedAnswer = "Cannot answer - JSON parse error";
    return;
  }

  String extractedText = "";
  if (transcriptionDoc["choices"][0]["message"]["content"]) {
    extractedText = transcriptionDoc["choices"][0]["message"]["content"].as<String>();
    lastTranscription = extractedText;
    Serial.println("====================");
    Serial.println("TRANSCRIBED QUESTION:");
    Serial.println("====================");
    Serial.println(extractedText);
    Serial.println("====================");
  } else {
    Serial.println("No content found in transcription response");
    Serial.println("Full response:");
    serializeJsonPretty(transcriptionDoc, Serial);
    lastTranscription = "No content found in transcription response";
    lastAnswer = "Cannot answer - no transcription content";
    lastExtractedAnswer = "Cannot answer - no transcription content";
    return;
  }

  // STEP 2: Answer the question
  Serial.println("\n--- STEP 2: Answering Question ---");
  String fullPrompt = String(answer_prompt) + "\n\n" + extractedText;
  String answerResponse;
  bool answerSuccess = sendTextRequest(fullPrompt, answerResponse);

  if (!answerSuccess) {
    Serial.println("Failed to get answer");
    lastAnswer = "Failed to get answer - API error";
    lastExtractedAnswer = "Failed to get answer - API error";
    return;
  }

  // Parse answer response
  DynamicJsonDocument answerDoc(8192);
  error = deserializeJson(answerDoc, answerResponse);

  if (error) {
    Serial.printf("Answer JSON parse error: %s\n", error.c_str());
    Serial.println("Raw response (first 500 chars):");
    Serial.println(answerResponse.substring(0, 500));
    lastAnswer = "JSON parse error: " + String(error.c_str());
    lastExtractedAnswer = "JSON parse error: " + String(error.c_str());
    return;
  }

  if (answerDoc["choices"][0]["message"]["content"]) {
    String answer = answerDoc["choices"][0]["message"]["content"].as<String>();
    lastAnswer = answer;
    lastExtractedAnswer = extractAnswerFromTags(answer);
    
    Serial.println("====================");
    Serial.println("FULL AI RESPONSE:");
    Serial.println("====================");
    Serial.println(answer);
    Serial.println("====================");
    Serial.println("EXTRACTED ANSWER:");
    Serial.println("====================");
    Serial.println(lastExtractedAnswer);
    Serial.println("====================");
  } else {
    Serial.println("No content found in answer response");
    Serial.println("Full response:");
    serializeJsonPretty(answerDoc, Serial);
    lastAnswer = "No content found in answer response";
    lastExtractedAnswer = "No content found in answer response";
  }

  Serial.println("=== Question Processing Complete ===\n");
  Serial.printf("View results at: http://%s/\n", WiFi.localIP().toString().c_str());
}

// --- Setup & Loop ---

void setup() {
  Serial.begin(115200);
  Serial.println("\nBooting ESP32 OCR Camera System...");

  // Check for PSRAM
  if (psramFound()) {
    Serial.println("PSRAM found and will be used for larger images");
  } else {
    Serial.println("No PSRAM found - using smaller image sizes");
  }

  // Connect to WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  int wifi_attempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_attempts < 30) {
    delay(1000);
    Serial.print(".");
    wifi_attempts++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi connection failed!");
    ESP.restart();
  }
  Serial.println("\nWiFi connected.");
  Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("Web Interface: http://%s/\n", WiFi.localIP().toString().c_str());

  // Start web server
  server.on("/", handleRoot);
  server.on("/image", handleImage);
  server.on("/capture", handleCapture);
  server.begin();
  Serial.println("Web server started.");
  Serial.println("🚀 System ready!");
  Serial.println("Type 'take' in Serial Monitor or use web interface to capture.");
}

void loop() {
  server.handleClient();
  handleSerialCommands();
  delay(10);
}
