#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <ESPmDNS.h>
#include "AudioFileSourceLittleFS.h"
#include "AudioFileSourceSD.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"

// ===== PIN CONFIGURATION =====
const int LED_PINS[4] = {14, 4, 2, 15};  // ✅ Pin yang lebih stabil

// ===== SD CARD Configuration =====
#define SD_CS          5
#define SD_MOSI        23
#define SD_MISO        19
#define SD_SCK         18

// ===== I2S Audio Configuration (MAX98357) =====
// ✅ Disesuaikan karena GPIO 27 dipakai untuk LED
#define I2S_BCLK      25
#define I2S_LRC       27
#define I2S_DOUT      26


// Audio objects
AudioGeneratorWAV *wav = NULL;
AudioFileSourceLittleFS *fileLittleFS = NULL;
AudioFileSourceSD *fileSD = NULL;
AudioOutputI2S *out = NULL;

// Audio file paths (WAV format)
const char* audioFiles[4][2] = {
  {"/audio/alarm1.wav", "/audio/warning1.wav"},
  {"/audio/alarm2.wav", "/audio/warning2.wav"},
  {"/audio/alarm3.wav", "/audio/warning3.wav"},
  {"/audio/alarm4.wav", "/audio/warning4.wav"}
};

// Default audio files
const char* defaultAudioFiles[2] = {
  "/audio/alarm.wav",
  "/audio/warning.wav"
};

// ===== WiFi Configuration =====
const char* ap_ssid = "Alarm_Timer_Setup";
const char* ap_password = "12345678";

// ===== Static IP Configuration =====
IPAddress local_IP(192, 168, 1, 100);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(8, 8, 4, 4);

// ===== mDNS Configuration =====
const char* mdns_hostname = "system-alarm";

// ===== WEB SERVER =====
WebServer server(80);
Preferences preferences;

// ===== TIMER STRUCTURE =====
struct Timer {
  bool running;
  bool paused;
  unsigned long startTime;
  unsigned long pauseTime;
  unsigned long duration;
  unsigned long remainingTime;
  bool warningTriggered;
  bool alarmTriggered;
  String playerName;
  int sessionCount;
  unsigned long totalPlayTime;
};

Timer timers[4];

// ===== COURT SETTINGS =====
struct CourtSettings {
  int volume;
  int warningTime;
  bool loopAlarm;
} courtSettings[4];

// ===== GLOBAL CONFIGURATION =====
struct Config {
  int globalVolume;
  int alarmDuration;
  int warningDuration;
  bool useSDCard;
} config;

// ===== ALARM STATE =====
struct AlarmState {
  bool active;
  unsigned long startTime;
  int duration;
  bool isWarning;
  int fieldIndex;
  bool looping;
} alarmStates[4];

// ===== WiFi Credentials =====
String saved_ssid = "";
String saved_password = "";
bool wifi_connected = false;

// ===== Audio State =====
bool audioPlaying = false;
bool audioSystemReady = false;
bool sdCardAvailable = false;
bool littleFSAvailable = false;

// ===== FUNCTION DECLARATIONS =====
void loadConfiguration();
void loadWiFiCredentials();
void setupServerRoutes();
void handleRoot();
void handleWiFiPage();
void handleStatus();
void handleGetConfig();
void handleSetDuration();
void handleStart();
void handlePause();
void handleStop();
void handleSettings();
void handleGlobalVolume();
void handleUpload();
void handleWiFiScan();
void handleWiFiConnect();
void handleWiFiStatus();
void handleSerialCommand();
void updateTimers();
void updateAlarms();
void updateAudioPlayback();
void triggerAlarm(int index, int durationSeconds, bool isWarning);
void playAudio(int fieldIndex, bool isWarning);
void stopAudio();
void activateLED(int index);
void deactivateLED(int index);

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== Futsal Timer System v5.1 Complete ===");

  // Initialize LED pins
  for (int i = 0; i < 4; i++) {
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], LOW);
  }
  Serial.println("LED pins initialized");

  // Initialize SD Card
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS)) {
    sdCardAvailable = true;
    Serial.println("SD Card mounted successfully");
  } else {
    Serial.println("SD Card Mount Failed");
    sdCardAvailable = false;
  }

  // Initialize LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed");
    littleFSAvailable = false;
  } else {
    littleFSAvailable = true;
    Serial.println("LittleFS mounted successfully");
  }

  // Initialize I2S Audio Output
  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(0.7);
  
  audioSystemReady = true;
  Serial.println("MAX98357 I2S Audio initialized");

  // Initialize Preferences
  preferences.begin("futsal-timer", false);
  
  // Load configuration
  loadConfiguration();
  loadWiFiCredentials();
  
  // Apply global volume
  if (out) {
    float gain = config.globalVolume / 100.0;
    out->SetGain(gain);
  }

  // Initialize timers
  for (int i = 0; i < 4; i++) {
    timers[i].running = false;
    timers[i].paused = false;
    timers[i].startTime = 0;
    timers[i].pauseTime = 0;
    timers[i].duration = 0;
    timers[i].remainingTime = 0;
    timers[i].warningTriggered = false;
    timers[i].alarmTriggered = false;
    timers[i].playerName = "";
    timers[i].sessionCount = 0;
    timers[i].totalPlayTime = 0;
    
    alarmStates[i].active = false;
    alarmStates[i].startTime = 0;
    alarmStates[i].duration = 0;
    alarmStates[i].isWarning = false;
    alarmStates[i].fieldIndex = i;
    alarmStates[i].looping = false;
    
    courtSettings[i].volume = preferences.getInt(("vol" + String(i)).c_str(), 70);
    courtSettings[i].warningTime = preferences.getInt(("warn" + String(i)).c_str(), 5);
    courtSettings[i].loopAlarm = preferences.getBool(("loop" + String(i)).c_str(), false);
  }

  // Try to connect to saved WiFi
  if (saved_ssid.length() > 0) {
    Serial.println("Attempting to connect to saved WiFi: " + saved_ssid);
    WiFi.mode(WIFI_STA);
    
    if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
      Serial.println("Failed to configure Static IP");
    }
    
    WiFi.begin(saved_ssid.c_str(), saved_password.c_str());
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      wifi_connected = true;
      Serial.println("\nWiFi Connected!");
      Serial.print("IP Address: ");
      Serial.println(WiFi.localIP());
      
      if (MDNS.begin(mdns_hostname)) {
        Serial.println("mDNS responder started");
        Serial.print("Access via: http://");
        Serial.print(mdns_hostname);
        Serial.println(".local");
        MDNS.addService("http", "tcp", 80);
      }
    } else {
      Serial.println("\nFailed to connect to saved WiFi");
      wifi_connected = false;
    }
  }

  if (!wifi_connected) {
    Serial.println("Starting Access Point mode...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid, ap_password);
    Serial.print("AP IP Address: ");
    Serial.println(WiFi.softAPIP());
  }

  setupServerRoutes();
  server.begin();
  Serial.println("HTTP server started");
  Serial.println("================================\n");
  
  Serial.println("Current Configuration:");
  Serial.println("  Alarm Duration: " + String(config.alarmDuration) + " seconds");
  Serial.println("  Warning Duration: " + String(config.warningDuration) + " seconds");
  Serial.println("  Global Volume: " + String(config.globalVolume) + "%");
}

// ===== LOOP =====
void loop() {
  server.handleClient();
  updateTimers();
  updateAlarms();
  updateAudioPlayback();
  
  if (Serial.available()) {
    handleSerialCommand();
  }
  
  delay(10);
}

// ===== LOAD CONFIGURATION =====
void loadConfiguration() {
  config.globalVolume = preferences.getInt("globalVol", 70);
  config.alarmDuration = preferences.getInt("alarmDur", 12);
  config.warningDuration = preferences.getInt("warnDur", 8);
  config.useSDCard = preferences.getBool("useSD", true);
}

// ===== LOAD WIFI CREDENTIALS =====
void loadWiFiCredentials() {
  saved_ssid = preferences.getString("wifi_ssid", "");
  saved_password = preferences.getString("wifi_pass", "");
}

// ===== SERIAL COMMAND HANDLER =====
void handleSerialCommand() {
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  
  if (cmd.startsWith("alarm:")) {
    int dur = cmd.substring(6).toInt();
    if (dur >= 1 && dur <= 60) {
      config.alarmDuration = dur;
      preferences.putInt("alarmDur", dur);
      Serial.println("✅ Alarm duration set to: " + String(dur) + "s");
    } else {
      Serial.println("❌ Invalid duration (1-60)");
    }
  }
  else if (cmd.startsWith("warning:")) {
    int dur = cmd.substring(8).toInt();
    if (dur >= 1 && dur <= 30) {
      config.warningDuration = dur;
      preferences.putInt("warnDur", dur);
      Serial.println("✅ Warning duration set to: " + String(dur) + "s");
    } else {
      Serial.println("❌ Invalid duration (1-30)");
    }
  }
  else if (cmd == "status" || cmd == "config") {
    Serial.println("\n=== Current Configuration ===");
    Serial.println("Alarm Duration: " + String(config.alarmDuration) + "s");
    Serial.println("Warning Duration: " + String(config.warningDuration) + "s");
    Serial.println("Global Volume: " + String(config.globalVolume) + "%");
    Serial.println("Use SD Card: " + String(config.useSDCard ? "Yes" : "No"));
    Serial.println("WiFi Connected: " + String(wifi_connected ? "Yes" : "No"));
    Serial.println("============================\n");
  }
  else if (cmd == "help") {
    Serial.println("\n=== Available Commands ===");
    Serial.println("alarm:<seconds>    - Set alarm duration (1-60)");
    Serial.println("warning:<seconds>  - Set warning duration (1-30)");
    Serial.println("status             - Show current config");
    Serial.println("help               - Show this help");
    Serial.println("=========================\n");
  }
}

// ===== SERVER ROUTES SETUP =====
void setupServerRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/wifi", HTTP_GET, handleWiFiPage);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/start", HTTP_POST, handleStart);
  server.on("/api/pause", HTTP_POST, handlePause);
  server.on("/api/stop", HTTP_POST, handleStop);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/globalVolume", HTTP_POST, handleGlobalVolume);
  server.on("/api/getConfig", HTTP_GET, handleGetConfig);
  server.on("/api/setDuration", HTTP_POST, handleSetDuration);
  server.on("/api/wifi/scan", HTTP_GET, handleWiFiScan);
  server.on("/api/wifi/connect", HTTP_POST, handleWiFiConnect);
  server.on("/api/wifi/status", HTTP_GET, handleWiFiStatus);
  
  server.on("/api/upload", HTTP_POST, 
    []() {
      server.send(200);
    }, 
    handleUpload
  );
  
  server.onNotFound([]() {
    server.send(404, "text/plain", "404: Not Found");
  });
}

// ===== HANDLER: Root =====
void handleRoot() {
  if (config.useSDCard && sdCardAvailable) {
    File file = SD.open("/index.html", FILE_READ);
    if (file) {
      server.streamFile(file, "text/html");
      file.close();
      return;
    }
  }
  
  if (littleFSAvailable) {
    File file = LittleFS.open("/index.html", "r");
    if (file) {
      server.streamFile(file, "text/html");
      file.close();
      return;
    }
  }
  
  server.send(500, "text/plain", "Failed to open HTML file");
}

// ===== HANDLER: WiFi Page =====
void handleWiFiPage() {
  if (config.useSDCard && sdCardAvailable) {
    File file = SD.open("/wifi.html", FILE_READ);
    if (file) {
      server.streamFile(file, "text/html");
      file.close();
      return;
    }
  }
  
  if (littleFSAvailable) {
    File file = LittleFS.open("/wifi.html", "r");
    if (file) {
      server.streamFile(file, "text/html");
      file.close();
      return;
    }
  }
  
  server.send(500, "text/plain", "Failed to open WiFi setup page");
}

// ===== HANDLER: Status =====
void handleStatus() {
  StaticJsonDocument<1024> doc;
  JsonArray courts = doc.to<JsonArray>();
  
  for (int i = 0; i < 4; i++) {
    JsonObject court = courts.createNestedObject();
    court["id"] = i;
    court["remaining"] = timers[i].remainingTime;
    court["isRunning"] = timers[i].running && !timers[i].paused;
    court["isPaused"] = timers[i].paused;
    court["playerName"] = timers[i].playerName;
    court["sessionCount"] = timers[i].sessionCount;
    court["totalPlayTime"] = timers[i].totalPlayTime;
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

// ===== HANDLER: Get Config =====
void handleGetConfig() {
  StaticJsonDocument<256> doc;
  
  doc["globalVolume"] = config.globalVolume;
  doc["alarmDuration"] = config.alarmDuration;
  doc["warningDuration"] = config.warningDuration;
  doc["useSDCard"] = config.useSDCard;
  doc["sdAvailable"] = sdCardAvailable;
  doc["littleFSAvailable"] = littleFSAvailable;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
  
  Serial.println("Config requested via API");
}

// ===== HANDLER: Set Duration =====
void handleSetDuration() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "No data received");
    return;
  }
  
  String body = server.arg("plain");
  StaticJsonDocument<128> doc;
  
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  
  int alarmDur = doc["alarmDuration"] | config.alarmDuration;
  int warnDur = doc["warningDuration"] | config.warningDuration;
  
  if (alarmDur < 1 || alarmDur > 60) {
    server.send(400, "text/plain", "Alarm duration must be 1-60 seconds");
    return;
  }
  
  if (warnDur < 1 || warnDur > 30) {
    server.send(400, "text/plain", "Warning duration must be 1-30 seconds");
    return;
  }
  
  config.alarmDuration = alarmDur;
  config.warningDuration = warnDur;
  
  preferences.putInt("alarmDur", alarmDur);
  preferences.putInt("warnDur", warnDur);
  
  Serial.println("✅ Duration updated:");
  Serial.println("   Alarm: " + String(alarmDur) + " seconds");
  Serial.println("   Warning: " + String(warnDur) + " seconds");
  
  server.send(200, "text/plain", "Duration updated successfully");
}

// ===== HANDLER: Start Timer =====
void handleStart() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "No data received");
    return;
  }
  
  String body = server.arg("plain");
  StaticJsonDocument<256> doc;
  
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  
  int courtId = doc["courtId"];
  int duration = doc["duration"];
  String playerName = doc["playerName"] | "";
  
  if (courtId < 0 || courtId >= 4) {
    server.send(400, "text/plain", "Invalid court ID");
    return;
  }
  
  timers[courtId].running = true;
  timers[courtId].paused = false;
  timers[courtId].startTime = millis();
  timers[courtId].duration = duration * 60 * 1000UL;
  timers[courtId].remainingTime = duration * 60;
  timers[courtId].warningTriggered = false;
  timers[courtId].alarmTriggered = false;
  timers[courtId].playerName = playerName;
  timers[courtId].sessionCount++;
  
  Serial.println("Court " + String(courtId + 1) + " started - Duration: " + String(duration) + " minutes");
  
  server.send(200, "text/plain", "Timer started");
}

// ===== HANDLER: Pause Timer =====
void handlePause() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "No data received");
    return;
  }
  
  String body = server.arg("plain");
  StaticJsonDocument<128> doc;
  
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  
  int courtId = doc["courtId"];
  
  if (courtId < 0 || courtId >= 4) {
    server.send(400, "text/plain", "Invalid court ID");
    return;
  }
  
  if (timers[courtId].running && !timers[courtId].paused) {
    timers[courtId].paused = true;
    timers[courtId].pauseTime = millis();
    Serial.println("Court " + String(courtId + 1) + " paused");
  } else if (timers[courtId].paused) {
    unsigned long pauseDuration = millis() - timers[courtId].pauseTime;
    timers[courtId].startTime += pauseDuration;
    timers[courtId].paused = false;
    Serial.println("Court " + String(courtId + 1) + " resumed");
  }
  
  server.send(200, "text/plain", "Timer toggled");
}

// ===== HANDLER: Stop Timer =====
void handleStop() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "No data received");
    return;
  }
  
  String body = server.arg("plain");
  StaticJsonDocument<128> doc;
  
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  
  int courtId = doc["courtId"];
  
  if (courtId < 0 || courtId >= 4) {
    server.send(400, "text/plain", "Invalid court ID");
    return;
  }
  
  timers[courtId].running = false;
  timers[courtId].paused = false;
  timers[courtId].remainingTime = 0;
  deactivateLED(courtId);
  stopAudio();
  
  Serial.println("Court " + String(courtId + 1) + " stopped");
  
  server.send(200, "text/plain", "Timer stopped");
}

// ===== HANDLER: Settings =====
void handleSettings() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "No data received");
    return;
  }
  
  String body = server.arg("plain");
  StaticJsonDocument<256> doc;
  
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  
  int courtId = doc["courtId"];
  
  if (courtId < 0 || courtId >= 4) {
    server.send(400, "text/plain", "Invalid court ID");
    return;
  }
  
  courtSettings[courtId].volume = doc["volume"] | 70;
  courtSettings[courtId].warningTime = doc["warningTime"] | 5;
  courtSettings[courtId].loopAlarm = doc["loopAlarm"] | false;
  
  preferences.putInt(("vol" + String(courtId)).c_str(), courtSettings[courtId].volume);
  preferences.putInt(("warn" + String(courtId)).c_str(), courtSettings[courtId].warningTime);
  preferences.putBool(("loop" + String(courtId)).c_str(), courtSettings[courtId].loopAlarm);
  
  Serial.println("Settings updated for Court " + String(courtId + 1));
  
  server.send(200, "text/plain", "Settings saved");
}

// ===== HANDLER: Global Volume =====
void handleGlobalVolume() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "No data received");
    return;
  }
  
  String body = server.arg("plain");
  StaticJsonDocument<128> doc;
  
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  
  config.globalVolume = doc["volume"] | 70;
  
  if (out) {
    float gain = config.globalVolume / 100.0;
    out->SetGain(gain);
  }
  
  preferences.putInt("globalVol", config.globalVolume);
  
  Serial.println("Global volume updated: " + String(config.globalVolume));
  
  server.send(200, "text/plain", "Volume updated");
}

// ===== HANDLER: Upload Audio =====
File uploadFile;
unsigned long uploadStartTime = 0;
size_t uploadedBytes = 0;
const size_t MAX_UPLOAD_SIZE = 10 * 1024 * 1024; // 10MB limit

// ✅ Validate filename security
bool isValidFilename(String filename) {
  // Remove path traversal attempts
  filename.replace("../", "");
  filename.replace("..\\", "");
  
  // Check if valid audio filename format
  filename.toLowerCase();
  if (!filename.endsWith(".wav")) {
    return false;
  }
  
  // Only allow specific filenames
  if (filename == "alarm.wav" || filename == "warning.wav") {
    return true;
  }
  
  // Check for alarm1-4.wav or warning1-4.wav
  if (filename.startsWith("alarm") || filename.startsWith("warning")) {
    char lastChar = filename.charAt(filename.indexOf(".wav") - 1);
    if (lastChar >= '1' && lastChar <= '4') {
      return true;
    }
  }
  
  return false;
}

void handleUpload() {
  HTTPUpload& upload = server.upload();
  
  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    filename.toLowerCase();
    
    // ✅ Security: Validate filename
    if (!isValidFilename(filename)) {
      Serial.println("❌ Invalid filename: " + filename);
      uploadFile = File();
      return;
    }
    
    // ✅ Security: Check file size (max 10MB)
    uploadStartTime = millis();
    uploadedBytes = 0;
    
    String filepath = "/audio/" + filename;
    Serial.println("📤 Upload started: " + filepath);
    
    // ✅ Create audio directory if not exists
    if (config.useSDCard && sdCardAvailable) {
      if (!SD.exists("/audio")) {
        if (!SD.mkdir("/audio")) {
          Serial.println("❌ Failed to create /audio directory");
          return;
        }
      }
      
      // ✅ Check if file exists and warn
      if (SD.exists(filepath)) {
        Serial.println("⚠️ File exists, will be overwritten: " + filepath);
      }
      
      uploadFile = SD.open(filepath, FILE_WRITE);
      if (!uploadFile) {
        Serial.println("❌ Failed to open file for writing on SD Card");
      }
    } else if (littleFSAvailable) {
      // ✅ Create directory in LittleFS
      File root = LittleFS.open("/audio", "r");
      if (!root) {
        if (!LittleFS.mkdir("/audio")) {
          Serial.println("❌ Failed to create /audio directory");
          return;
        }
      } else {
        root.close();
      }
      
      // ✅ Check if file exists
      if (LittleFS.exists(filepath)) {
        Serial.println("⚠️ File exists, will be overwritten: " + filepath);
      }
      
      uploadFile = LittleFS.open(filepath, "w");
      if (!uploadFile) {
        Serial.println("❌ Failed to open file for writing on LittleFS");
      }
    } else {
      Serial.println("❌ No storage available");
    }
  } 
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      // ✅ Security: Check total size limit
      uploadedBytes += upload.currentSize;
      if (uploadedBytes > MAX_UPLOAD_SIZE) {
        Serial.println("❌ File too large, aborting upload");
        uploadFile.close();
        uploadFile = File();
        server.send(413, "text/plain", "File too large (max 10MB)");
        return;
      }
      
      size_t written = uploadFile.write(upload.buf, upload.currentSize);
      if (written != upload.currentSize) {
        Serial.println("❌ Write error");
      }
    }
  } 
  else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
      
      unsigned long uploadDuration = millis() - uploadStartTime;
      float uploadSpeed = (uploadedBytes / 1024.0) / (uploadDuration / 1000.0);
      
      Serial.printf("✅ Upload finished: %s\n", upload.filename.c_str());
      Serial.printf("   Size: %d bytes (%.2f KB)\n", upload.totalSize, upload.totalSize / 1024.0);
      Serial.printf("   Duration: %lu ms\n", uploadDuration);
      Serial.printf("   Speed: %.2f KB/s\n", uploadSpeed);
      
      // ✅ Verify file was written correctly
      String filepath = "/audio/" + String(upload.filename);
      filepath.toLowerCase();
      
      bool verified = false;
      if (config.useSDCard && sdCardAvailable) {
        if (SD.exists(filepath)) {
          File checkFile = SD.open(filepath, FILE_READ);
          if (checkFile && checkFile.size() == upload.totalSize) {
            verified = true;
          }
          checkFile.close();
        }
      } else if (littleFSAvailable) {
        if (LittleFS.exists(filepath)) {
          File checkFile = LittleFS.open(filepath, "r");
          if (checkFile && checkFile.size() == upload.totalSize) {
            verified = true;
          }
          checkFile.close();
        }
      }
      
      if (verified) {
        server.send(200, "application/json", 
          "{\"success\":true,\"message\":\"Upload successful\",\"size\":" + String(upload.totalSize) + "}");
      } else {
        server.send(500, "text/plain", "Upload verification failed");
      }
    } else {
      Serial.println("❌ Upload failed - file not opened");
      server.send(500, "text/plain", "Upload failed - invalid file");
    }
    
    uploadFile = File();
    uploadedBytes = 0;
  }
  else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) {
      uploadFile.close();
      uploadFile = File();
    }
    uploadedBytes = 0;
    Serial.println("❌ Upload aborted");
    server.send(500, "text/plain", "Upload aborted");
  }
}

// ===== HANDLER: WiFi Scan =====
void handleWiFiScan() {
  Serial.println("Scanning WiFi networks...");
  int n = WiFi.scanNetworks();
  
  StaticJsonDocument<2048> doc;
  JsonArray networks = doc.to<JsonArray>();
  
  for (int i = 0; i < n; i++) {
    JsonObject network = networks.createNestedObject();
    network["ssid"] = WiFi.SSID(i);
    network["rssi"] = WiFi.RSSI(i);
    network["encryption"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "Open" : "Secured";
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
  
  WiFi.scanDelete();
}

// ===== HANDLER: WiFi Connect =====
void handleWiFiConnect() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "No data received");
    return;
  }
  
  String body = server.arg("plain");
  StaticJsonDocument<256> doc;
  
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  
  String ssid = doc["ssid"];
  String password = doc["password"];
  
  preferences.putString("wifi_ssid", ssid);
  preferences.putString("wifi_pass", password);
  
  Serial.println("WiFi credentials saved. Restarting...");
  
  server.send(200, "text/plain", "Credentials saved. ESP32 will restart.");
  
  delay(1000);
  ESP.restart();
}

// ===== HANDLER: WiFi Status =====
void handleWiFiStatus() {
  StaticJsonDocument<512> doc;
  
  doc["connected"] = (WiFi.status() == WL_CONNECTED);
  
  if (WiFi.status() == WL_CONNECTED) {
    doc["ssid"] = WiFi.SSID();
    doc["ip"] = WiFi.localIP().toString();
    doc["rssi"] = WiFi.RSSI();
    doc["hostname"] = mdns_hostname;
  } else {
    doc["ap_ssid"] = ap_ssid;
    doc["ap_ip"] = WiFi.softAPIP().toString();
    doc["clients"] = WiFi.softAPgetStationNum();
    doc["saved_ssid"] = saved_ssid;
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

// ===== UPDATE TIMERS =====
void updateTimers() {
  unsigned long currentMillis = millis();
  
  for (int i = 0; i < 4; i++) {
    if (!timers[i].running || timers[i].paused) continue;
    
    unsigned long elapsed = currentMillis - timers[i].startTime;
    long remaining = (timers[i].duration - elapsed) / 1000;
    
    if (remaining < 0) remaining = 0;
    timers[i].remainingTime = remaining;
    timers[i].totalPlayTime = elapsed / 1000;
    
    // Warning alarm
    if (!timers[i].warningTriggered && 
        remaining <= (courtSettings[i].warningTime * 60) && remaining > 0) {
      timers[i].warningTriggered = true;
      triggerAlarm(i, config.warningDuration, true);
      Serial.println("⚠️ Warning alarm Court " + String(i + 1));
    }
    
    // Time up alarm
    if (!timers[i].alarmTriggered && remaining <= 0) {
      timers[i].alarmTriggered = true;
      timers[i].running = false;
      triggerAlarm(i, config.alarmDuration, false);
      Serial.println("🔔 Time up alarm Court " + String(i + 1));
    }
  }
}

// ===== UPDATE ALARMS =====
void updateAlarms() {
  unsigned long currentMillis = millis();
  
  for (int i = 0; i < 4; i++) {
    if (!alarmStates[i].active) continue;
    
    unsigned long elapsed = currentMillis - alarmStates[i].startTime;
    
    if (elapsed >= alarmStates[i].duration) {
      if (alarmStates[i].looping && courtSettings[i].loopAlarm) {
        alarmStates[i].startTime = currentMillis;
        playAudio(i, alarmStates[i].isWarning);
        Serial.println("🔁 Loop alarm Court " + String(i + 1));
      } else {
        alarmStates[i].active = false;
        deactivateLED(i);
        stopAudio();
      }
    }
  }
}

// ===== UPDATE AUDIO PLAYBACK =====
void updateAudioPlayback() {
  if (audioPlaying && wav) {
    if (wav->isRunning()) {
      if (!wav->loop()) {
        stopAudio();
      }
    } else {
      stopAudio();
    }
  }
}

// ===== TRIGGER ALARM =====
void triggerAlarm(int index, int durationSeconds, bool isWarning) {
  alarmStates[index].active = true;
  alarmStates[index].startTime = millis();
  alarmStates[index].duration = durationSeconds * 1000UL;
  alarmStates[index].isWarning = isWarning;
  alarmStates[index].fieldIndex = index;
  alarmStates[index].looping = courtSettings[index].loopAlarm;
  
  activateLED(index);
  playAudio(index, isWarning);
  
  Serial.println("🎵 Playing " + String(isWarning ? "WARNING" : "ALARM") + 
                 " for " + String(durationSeconds) + "s");
}

// ===== PLAY AUDIO =====
void playAudio(int fieldIndex, bool isWarning) {
  if (!audioSystemReady) {
    Serial.println("❌ Audio system not ready");
    return;
  }
  
  stopAudio();
  
  const char* audioPath = nullptr;
  int audioType = isWarning ? 1 : 0;
  bool useSD = config.useSDCard && sdCardAvailable;
  
  // Try field-specific file first
  audioPath = audioFiles[fieldIndex][audioType];
  
  if (useSD) {
    if (SD.exists(audioPath)) {
      fileSD = new AudioFileSourceSD(audioPath);
      wav = new AudioGeneratorWAV();
      if (wav->begin(fileSD, out)) {
        audioPlaying = true;
        Serial.println("🎵 Playing from SD: " + String(audioPath));
        return;
      } else {
        delete wav;
        delete fileSD;
        wav = NULL;
        fileSD = NULL;
      }
    }
  } else if (littleFSAvailable) {
    if (LittleFS.exists(audioPath)) {
      fileLittleFS = new AudioFileSourceLittleFS(audioPath);
      wav = new AudioGeneratorWAV();
      if (wav->begin(fileLittleFS, out)) {
        audioPlaying = true;
        Serial.println("🎵 Playing from LittleFS: " + String(audioPath));
        return;
      } else {
        delete wav;
        delete fileLittleFS;
        wav = NULL;
        fileLittleFS = NULL;
      }
    }
  }
  
  // Try default file
  audioPath = defaultAudioFiles[audioType];
  
  if (useSD) {
    if (SD.exists(audioPath)) {
      fileSD = new AudioFileSourceSD(audioPath);
      wav = new AudioGeneratorWAV();
      if (wav->begin(fileSD, out)) {
        audioPlaying = true;
        Serial.println("🎵 Playing default from SD: " + String(audioPath));
        return;
      } else {
        delete wav;
        delete fileSD;
        wav = NULL;
        fileSD = NULL;
      }
    }
  } else if (littleFSAvailable) {
    if (LittleFS.exists(audioPath)) {
      fileLittleFS = new AudioFileSourceLittleFS(audioPath);
      wav = new AudioGeneratorWAV();
      if (wav->begin(fileLittleFS, out)) {
        audioPlaying = true;
        Serial.println("🎵 Playing default from LittleFS: " + String(audioPath));
        return;
      } else {
        delete wav;
        delete fileLittleFS;
        wav = NULL;
        fileLittleFS = NULL;
      }
    }
  }
  
  Serial.println("❌ No audio file found for playback");
}

// ===== STOP AUDIO =====
void stopAudio() {
  if (wav) {
    if (wav->isRunning()) {
      wav->stop();
    }
    delete wav;
    wav = NULL;
  }
  
  if (fileSD) {
    delete fileSD;
    fileSD = NULL;
  }
  
  if (fileLittleFS) {
    delete fileLittleFS;
    fileLittleFS = NULL;
  }
  
  audioPlaying = false;
}

// ===== ACTIVATE LED =====
void activateLED(int index) {
  if (index >= 0 && index < 4) {
    digitalWrite(LED_PINS[index], HIGH);
    Serial.println("💡 LED " + String(index + 1) + " ON");
  }
}

// ===== DEACTIVATE LED =====
void deactivateLED(int index) {
  if (index >= 0 && index < 4) {
    digitalWrite(LED_PINS[index], LOW);
    Serial.println("💡 LED " + String(index + 1) + " OFF");
  }
}
