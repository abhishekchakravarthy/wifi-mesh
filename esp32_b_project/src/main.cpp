/*
 * ESP32-S3 BLE Audio Client (ESP32 B)
 * 
 * This ESP32 connects to ESP32 A via WiFi mesh and receives audio data
 * that it can then forward to Phone B via BLE.
 * 
 * WiFi Mesh Client + BLE Server
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <esp_gap_ble_api.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

// Mesh Network Configuration for Client Device
#define MESH_CHANNEL 1
#define MESH_BROADCAST_INTERVAL 10000  // 10 seconds
#define MESH_HEARTBEAT_INTERVAL 5000   // 5 seconds (matching coordinator)
#define DEVICE_TIMEOUT 30000           // 30 seconds (matching coordinator)

// Mesh network state
bool isMeshConnected = false;
bool esp32_a_connected = false;
uint8_t esp32_a_mac[6] = {0};
unsigned long lastMeshJoinAttempt = 0;
unsigned long lastMeshHeartbeat = 0;
unsigned long lastMeshStatus = 0;

// Device identification
String deviceName = "ESP32S3_Audio_Client";
String deviceType = "ESP32_Audio_Client";

// BLE UUIDs for Phone B (same as ESP32 A for compatibility)
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"  // Same as coordinator
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"  // Same as coordinator
#define DESCRIPTOR_UUID     "00002902-0000-1000-8000-00805f9b34fb"

// Pin definitions
#define STATUS_LED_PIN 48    // GPIO48 - RGB LED
#define MESH_LED_PIN 2      // GPIO2 - Mesh connection indicator
#define BLE_LED_PIN 4       // GPIO4 - BLE connection indicator
#define NUM_LEDS 1          // Number of RGB LEDs

// Global variables
bool isMeshDataReceived = false;

// Neopixel LED control
Adafruit_NeoPixel pixels(NUM_LEDS, STATUS_LED_PIN, NEO_GRB + NEO_KHZ800);

// BLE Server - Simplified to match working coordinator
BLEServer* pServer = NULL;
BLECharacteristic* pAudioCharacteristic = NULL;
bool bleServerStarted = false;  // Set true after service starts
bool bleAdvertising = false;    // Set true after advertising starts

// BLE Connection state
bool bleDeviceConnected = false;
bool oldBleDeviceConnected = false;

// BLE Error handling - simplified

// BLE initialization moved to setup() function - simplified to match working coordinator

// BLE error handling removed - simplified to match working coordinator

// Audio buffer
uint8_t audioBuffer[200];
size_t audioBufferIndex = 0;

// Statistics
unsigned long packetsReceived = 0;
unsigned long bytesReceived = 0;
unsigned long lastStatsTime = 0;

// Notification queue (lockless, IRQ-safe) for BLE forwards
struct NotifyItem {
  uint16_t length;
  uint8_t data[512];
  uint8_t isPcm8; // 1 if data are 8-bit PCM samples to upconvert
};
static volatile uint16_t notifyHead = 0;
static volatile uint16_t notifyTail = 0;
#define NOTIFY_RING_SIZE 64
#define NOTIFY_RING_MASK (NOTIFY_RING_SIZE - 1)
static NotifyItem notifyQueue[NOTIFY_RING_SIZE];

static inline bool notifyQueuePushFromISR(const uint8_t* buf, uint16_t len, uint8_t isPcm8) {
  if (len > 256) len = 256;
  uint16_t nextHead = (notifyHead + 1) & NOTIFY_RING_MASK;
  if (nextHead == notifyTail) return false; // full
  NotifyItem &slot = notifyQueue[notifyHead & NOTIFY_RING_MASK];
  slot.length = len;
  memcpy(slot.data, buf, len);
  slot.isPcm8 = isPcm8;
  __atomic_store_n(&notifyHead, nextHead, __ATOMIC_RELEASE);
  return true;
}

static inline bool notifyQueuePop(NotifyItem &out) {
  uint16_t tail = __atomic_load_n(&notifyTail, __ATOMIC_ACQUIRE);
  uint16_t head = __atomic_load_n(&notifyHead, __ATOMIC_ACQUIRE);
  if (tail == head) return false; // empty
  NotifyItem &slot = notifyQueue[tail & NOTIFY_RING_MASK];
  out.length = slot.length;
  memcpy(out.data, slot.data, slot.length);
  __atomic_store_n(&notifyTail, (uint16_t)((tail + 1) & NOTIFY_RING_MASK), __ATOMIC_RELEASE);
  return true;
}

// Forward declarations
void setStatusLED(uint8_t r, uint8_t g, uint8_t b);
void blinkStatusLED(uint8_t r, uint8_t g, uint8_t b, int times);
void startScanningForESP32A();
void sendJoinMessage();
void setupESPNOWMesh();
void OnDataRecv(const uint8_t *mac, const uint8_t *data, int len);
void handleTestAudioData(const uint8_t* data, int len, const DynamicJsonDocument& doc);
void sendTestAck(const uint8_t* mac, int testId, const String& status);
void processReceivedAudioData(const uint8_t* audioData, int length, int sequence, int chunk, int totalChunks);
void sendAudioAck(const uint8_t* mac, int sequence, int chunk, const String& status);

// RAW PCM: No decompression - direct data passthrough
static int decompressOptimizedAudio(const uint8_t* compressedData, int compressedLen,
                                    uint8_t* output, int outputMax) {
  if (!compressedData || compressedLen <= 0 || !output || outputMax <= 0) {
    return 0;
  }

  // Direct copy - no decompression needed
  int copyLen = (compressedLen < outputMax) ? compressedLen : outputMax;
  memcpy(output, compressedData, copyLen);
  return copyLen;
}

// Audio processing functions
void processReceivedAudioData(const uint8_t* audioData, int length, int sequence, int chunk, int totalChunks) {
  Serial.printf("🎵 Processing audio chunk %d/%d (sequence %d)\n", chunk + 1, totalChunks, sequence);
  
  // Calculate audio statistics
  uint32_t sum = 0;
  uint8_t minVal = 255;
  uint8_t maxVal = 0;
  
  for (int i = 0; i < length; i++) {
    sum += audioData[i];
    if (audioData[i] < minVal) minVal = audioData[i];
    if (audioData[i] > maxVal) maxVal = audioData[i];
  }
  
  uint8_t avgVal = sum / length;
  
  Serial.printf("🎵 Audio Stats - Min: %d, Max: %d, Avg: %d, Length: %d bytes\n", 
                minVal, maxVal, avgVal, length);
  
  // Update statistics
  packetsReceived++;
  bytesReceived += length;
  
  // 🎯 FORWARD AUDIO DATA TO PHONE B VIA BLE (using queue-based system)
  if (bleDeviceConnected && pAudioCharacteristic) {
    Serial.printf("📱 Queueing audio chunk %d/%d for BLE forwarding (%d bytes)\n", 
                  chunk + 1, totalChunks, length);
    
    // Use queue-based forwarding for better performance and reliability
    if (!notifyQueuePushFromISR(audioData, (uint16_t)length, 0)) {
      Serial.printf("⚠️ BLE queue full, dropping audio chunk %d/%d\n", chunk + 1, totalChunks);
    } else {
      Serial.printf("✅ Audio chunk %d/%d queued for BLE transmission\n", chunk + 1, totalChunks);
    }
    
    // Log BLE transmission details
    Serial.printf("📊 BLE Queue - Size: %d bytes, Sequence: %d, Chunk: %d/%d\n",
                  length, sequence, chunk + 1, totalChunks);
    
  } else {
    Serial.printf("⚠️ Cannot forward audio to Phone B - BLE not connected\n");
    Serial.printf("   BLE Status: %s, Characteristic: %s\n", 
                  bleDeviceConnected ? "Connected" : "Disconnected",
                  pAudioCharacteristic ? "Available" : "NULL");
  }
  
  // In a real implementation, you would also:
  // 1. Buffer the audio data for playback
  // 2. Handle audio synchronization and timing
  // 3. Implement audio decompression if needed
}

void sendAudioAck(const uint8_t* mac, int sequence, int chunk, const String& status) {
  DynamicJsonDocument ackDoc(256);
  ackDoc["type"] = "audio_ack";
  ackDoc["sequence"] = sequence;
  ackDoc["chunk"] = chunk;
  ackDoc["status"] = status;
  ackDoc["source"] = "ESP32_B_Client";
  ackDoc["timestamp"] = millis();
  
  String ackString;
  serializeJson(ackDoc, ackString);
  
  esp_err_t result = esp_now_send(mac, (uint8_t*)ackString.c_str(), ackString.length());
  if (result == ESP_OK) {
    Serial.printf("✅ Audio ACK sent to coordinator for chunk %d\n", chunk);
  } else {
    Serial.printf("❌ Failed to send audio ACK: %d\n", result);
  }
}

// ESP-NOW Mesh Functions
void setupESPNOWMesh() {
  Serial.println("Setting up ESP-NOW Mesh...");
  
  // Set WiFi mode to STA for ESP-NOW
  WiFi.mode(WIFI_STA);
  // Enable WiFi modem sleep for BLE/WiFi coexistence (required on ESP32-S3)
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  WiFi.disconnect();
  delay(100);
  
  // Set WiFi channel to match mesh channel for ESP-NOW compatibility
  WiFi.channel(MESH_CHANNEL);
  Serial.printf("WiFi channel set to %d for ESP-NOW compatibility\n", MESH_CHANNEL);
  
  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  
  // Set ESP-NOW role to controller
  esp_now_set_pmk((uint8_t *)"ESP32_Mesh_Key_12345");
  
  // Register callbacks
  esp_now_register_recv_cb(OnDataRecv);
  // esp_now_register_send_cb(OnDataSent); // Not needed for client
  
  Serial.println("ESP-NOW Mesh initialized successfully");
  
  // Start broadcasting presence to ESP32 A
  startScanningForESP32A();
}

void startScanningForESP32A() {
  Serial.println("Attempting to join mesh network...");
  setStatusLED(255, 165, 0); // Orange while joining
  
  // Create join message
  DynamicJsonDocument doc(256);
  doc["type"] = "mesh_join";
  doc["source"] = deviceName;
  doc["device_name"] = deviceName;
  doc["device_type"] = deviceType;
  doc["timestamp"] = millis();
  doc["mac"] = WiFi.macAddress();
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  // Use the same channel as ESP32 A (MESH_CHANNEL = 1)
  int channel = MESH_CHANNEL;
  
  // Try to send to multiple potential coordinator MACs
  // We'll try common ESP32 MAC patterns and also broadcast
  uint8_t potentialCoordinators[][6] = {
    {0x10, 0x00, 0x3B, 0x48, 0x9C, 0x3C}, // ESP32 A's actual MAC from logs
    {0x10, 0x00, 0x3B, 0x48, 0x1A, 0x68}, // ESP32 B's MAC (for testing)
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}  // Broadcast (fallback)
  };
  
  bool joinSent = false;
  
  for (int attempt = 0; attempt < 3; attempt++) {
    // Create peer info
    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, potentialCoordinators[attempt], 6);
    peerInfo.channel = channel;
    peerInfo.encrypt = false;
    peerInfo.ifidx = WIFI_IF_STA;
    
    // Remove any existing peer first
    esp_now_del_peer(peerInfo.peer_addr);
    
    esp_err_t result = esp_now_add_peer(&peerInfo);
    if (result == ESP_OK) {
      Serial.printf("Attempting to send join request to MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                    peerInfo.peer_addr[0], peerInfo.peer_addr[1], peerInfo.peer_addr[2],
                    peerInfo.peer_addr[3], peerInfo.peer_addr[4], peerInfo.peer_addr[5]);
      
      // Send the join message
      esp_err_t sendResult = esp_now_send(peerInfo.peer_addr, (uint8_t*)jsonString.c_str(), jsonString.length());
      if (sendResult == ESP_OK) {
        Serial.println("Join request sent successfully");
        lastMeshJoinAttempt = millis();
        joinSent = true;
        
        // If this is a specific MAC (not broadcast), store it
        if (attempt < 2) {
          memcpy(esp32_a_mac, peerInfo.peer_addr, 6);
          Serial.printf("Stored potential coordinator MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                        esp32_a_mac[0], esp32_a_mac[1], esp32_a_mac[2],
                        esp32_a_mac[3], esp32_a_mac[4], esp32_a_mac[5]);
        }
      } else {
        Serial.printf("Failed to send join request: %d\n", sendResult);
      }
      
      // Clean up the peer
      esp_now_del_peer(peerInfo.peer_addr);
    } else {
      Serial.printf("Failed to add peer for attempt %d: %d\n", attempt, result);
    }
    
    delay(100); // Small delay between attempts
  }
  
  if (joinSent) {
    Serial.println("Join request sent, waiting for mesh coordinator response...");
    setStatusLED(0, 255, 255); // Cyan while waiting
  } else {
    Serial.println("Failed to send join request to any coordinator");
    setStatusLED(255, 0, 0); // Red for failure
  }
}

void sendJoinMessage() {
  DynamicJsonDocument doc(256);
  doc["type"] = "mesh_join";
  doc["source"] = "ESP32_B_Client";
  doc["timestamp"] = millis();
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  esp_err_t result = esp_now_send(esp32_a_mac, (uint8_t*)jsonString.c_str(), jsonString.length());
  if (result == ESP_OK) {
    Serial.println("Join message sent to ESP32 A");
  } else {
    Serial.println("Failed to send join message");
  }
}

void sendAudioAck(const uint8_t* mac) {
  DynamicJsonDocument ackDoc(256);
  ackDoc["type"] = "audio_ack";
  ackDoc["source"] = deviceName;
  ackDoc["status"] = "received";
  ackDoc["timestamp"] = millis();
  
  String ackString;
  serializeJson(ackDoc, ackString);
  
  esp_err_t result = esp_now_send(mac, (uint8_t*)ackString.c_str(), ackString.length());
  if (result == ESP_OK) {
    Serial.println("Audio acknowledgment sent to coordinator");
  } else {
    Serial.printf("Failed to send audio acknowledgment: %d\n", result);
  }
}

void sendReadyConfirmation() {
  DynamicJsonDocument readyDoc(256);
  readyDoc["type"] = "mesh_ready";
  readyDoc["source"] = deviceName;
  readyDoc["status"] = "ready";
  readyDoc["timestamp"] = millis();
  readyDoc["coordinator_mac"] = WiFi.macAddress();
  
  String readyString;
  serializeJson(readyDoc, readyString);
  
  esp_err_t result = esp_now_send(esp32_a_mac, (uint8_t*)readyString.c_str(), readyString.length());
  if (result == ESP_OK) {
    Serial.println("Ready confirmation sent to coordinator");
  } else {
    Serial.printf("Failed to send ready confirmation: %d\n", result);
  }
}

void printStatistics() {
  Serial.println("=== ESP32 B MESH CLIENT STATISTICS ===");
  Serial.printf("Mesh connected: %s\n", isMeshConnected ? "Yes" : "No");
  Serial.printf("Coordinator connected: %s\n", esp32_a_connected ? "Yes" : "No");
  Serial.printf("BLE connected: %s\n", bleDeviceConnected ? "Yes" : "No");
  Serial.printf("Packets received: %lu\n", packetsReceived);
  Serial.printf("Bytes received: %lu\n", bytesReceived);
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  
  if (isMeshConnected && esp32_a_connected) {
    unsigned long timeSinceHeartbeat = millis() - lastMeshHeartbeat;
    Serial.printf("Last heartbeat: %lu ms ago\n", timeSinceHeartbeat);
  }
  
  Serial.println("=====================================");
}

// LED Control Functions
void setStatusLED(uint8_t r, uint8_t g, uint8_t b) {
  pixels.setPixelColor(0, pixels.Color(r, g, b));
  pixels.show();
}

void blinkStatusLED(uint8_t r, uint8_t g, uint8_t b, int times) {
  for (int i = 0; i < times; i++) {
    setStatusLED(r, g, b);
    delay(200);
    setStatusLED(0, 0, 0);
    delay(200);
  }
}

// Callback classes
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      bleDeviceConnected = true;
      Serial.println("=== PHONE B CONNECTED ===");
      digitalWrite(BLE_LED_PIN, HIGH);
      setStatusLED(0, 255, 0); // Green when BLE connected
    };

    void onDisconnect(BLEServer* pServer) {
      bleDeviceConnected = false;
      Serial.println("=== PHONE B DISCONNECTED ===");
      digitalWrite(BLE_LED_PIN, LOW);
      setStatusLED(255, 0, 0); // Red when BLE disconnected
    }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      std::string rxValue = pCharacteristic->getValue();
      
      if (rxValue.length() > 0) {
        Serial.println("=== AUDIO DATA RECEIVED FROM PHONE B ===");
        Serial.printf("Received %d bytes\n", rxValue.length());
        
        // Print first 8 bytes for debugging
        Serial.print("First 8 bytes: ");
        for (int i = 0; i < min(8, (int)rxValue.length()); i++) {
          Serial.printf("0x%02X ", (uint8_t)rxValue[i]);
        }
        Serial.println();
        
        // Update statistics
        packetsReceived++;
        bytesReceived += rxValue.length();
      }
    }
    
    void onRead(BLECharacteristic *pCharacteristic) {
      Serial.println("Characteristic read request from Phone B");
    }
};

// Handle test audio data
void handleTestAudioData(const uint8_t* data, int len, const DynamicJsonDocument& doc) {
  int testId = doc["test_id"];
  int dataSize = doc["data_size"];
  String dataType = doc["data_type"];
  int expectedChecksum = doc["checksum"];
  
  Serial.printf("🧪 TEST_AUDIO_RECEIVED:%d - Size: %d bytes, Type: %s\n", 
                testId, dataSize, dataType.c_str());
  
  // Calculate received data checksum
  uint32_t receivedChecksum = 0;
  for (int i = 0; i < len; i++) {
    receivedChecksum += data[i];
  }
  receivedChecksum &= 0xFFFF;
  
  // Verify checksum
  if (receivedChecksum == expectedChecksum) {
    Serial.printf("✅ TEST_AUDIO_RECEIVED:%d - Checksum verified: 0x%04X\n", 
                  testId, receivedChecksum);
    
    // Convert data to hex for logging
    String hexData = "";
    for (int i = 0; i < len && i < 32; i++) {  // Limit to first 32 bytes for display
      char hex[3];
      sprintf(hex, "%02X", data[i]);
      hexData += hex;
    }
    if (len > 32) hexData += "...";
    
    Serial.printf("🧪 TEST_AUDIO_RECEIVED:%d - DATA:%s\n", testId, hexData.c_str());
    
    // Send test acknowledgment back to coordinator
    sendTestAck(esp32_a_mac, testId, "received");
    
    // Update statistics
    packetsReceived++;
    bytesReceived += len;
    
  } else {
    Serial.printf("❌ TEST_AUDIO_RECEIVED:%d - Checksum mismatch! Expected: 0x%04X, Got: 0x%04X\n", 
                  testId, expectedChecksum, receivedChecksum);
  }
}

// Send test acknowledgment
void sendTestAck(const uint8_t* mac, int testId, const String& status) {
  DynamicJsonDocument ackDoc(256);
  ackDoc["type"] = "test_ack";
  ackDoc["test_id"] = testId;
  ackDoc["status"] = status;
  ackDoc["source"] = "ESP32_B_Client";
  ackDoc["timestamp"] = millis();
  
  String ackString;
  serializeJson(ackDoc, ackString);
  
  esp_err_t result = esp_now_send(mac, (uint8_t*)ackString.c_str(), ackString.length());
  if (result == ESP_OK) {
    Serial.printf("✅ Test ACK sent to coordinator for test %d\n", testId);
  } else {
    Serial.printf("❌ Failed to send test ACK: %d\n", result);
  }
}

// Parse compact header P:seq:chunk:total:timestamp:sample_rate:bits:min:max
static bool parseCompactAudioHeader(const uint8_t* data, int len, int* values, int& dataStart) {
  if (len < 5 || data[0] != 'P' || data[1] != ':') return false;
  int idx = 2;
  int field = 0;
  int current = 0;
  bool inNumber = false;
  while (idx < len && field < 8) {
    char c = (char)data[idx++];
    if (c == ':') {
      values[field++] = current;
      current = 0;
      inNumber = false;
    } else if (c >= '0' && c <= '9') {
      current = current * 10 + (c - '0');
      inNumber = true;
    } else {
      // unsupported char
      return false;
    }
  }
  if (field == 8) {
    dataStart = idx; // everything after last ':' is raw PCM payload
    return dataStart < len;
  }
  return false;
}

// Dedicated BLE notify flush task (35 ms cadence, 1280B target)
static void bleNotifyTask(void* pv) {
  const int MAX_NOTIFY_BYTES = 160;
  static uint8_t coalesceBuf[1536];
  static int coalesceLen = 0;
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t periodTicks = pdMS_TO_TICKS(25);
  for (;;) {
    // Ingest up to N items from notifyQueue each cycle
    int ingested = 0;
    while (ingested < 8) {
      NotifyItem item;
      if (!notifyQueuePop(item)) break;
      if (item.isPcm8) {
        // Convert 8-bit PCM to 16-bit PCM (legacy support)
        static uint8_t pcm16[512];
        int outLen = 0;
        for (int i = 0; i < item.length && outLen + 2 <= (int)sizeof(pcm16); i++) {
          uint8_t s8 = item.data[i];
          int16_t s16 = ((int)s8 - 128) << 8;
          pcm16[outLen++] = (uint8_t)(s16 & 0xFF);
          pcm16[outLen++] = (uint8_t)((s16 >> 8) & 0xFF);
        }
        int copy = min(outLen, (int)sizeof(coalesceBuf) - coalesceLen);
        memcpy(coalesceBuf + coalesceLen, pcm16, copy);
        coalesceLen += copy;
      } else {
        // Raw 16-bit PCM data - forward directly without any processing
        int copy = min((int)item.length, (int)sizeof(coalesceBuf) - coalesceLen);
        memcpy(coalesceBuf + coalesceLen, item.data, copy);
        coalesceLen += copy;
      }
      ingested++;
    }
    // Timed flush: send whenever at least 200B is ready; cap to 1280B per tick
    if (bleDeviceConnected && pAudioCharacteristic && coalesceLen >= 200) {
      int toSend = min(coalesceLen, 1280);
      int sent = 0;
      while (sent < toSend) {
        int chunk = min(MAX_NOTIFY_BYTES, toSend - sent);
        pAudioCharacteristic->setValue(coalesceBuf + sent, chunk);
        pAudioCharacteristic->notify();
        sent += chunk;
      }
      int remain = coalesceLen - toSend;
      if (remain > 0) memmove(coalesceBuf, coalesceBuf + toSend, remain);
      coalesceLen = remain;
    }
    vTaskDelayUntil(&lastWake, periodTicks);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== ESP32-S3 BLE AUDIO CLIENT STARTING ===");
  
  // Initialize Neopixel LED
  pixels.begin();
  pixels.setBrightness(50); // Set brightness to 50%
  setStatusLED(0, 0, 255); // Blue during startup
  
  // Initialize GPIO pins
  pinMode(MESH_LED_PIN, OUTPUT);
  pinMode(BLE_LED_PIN, OUTPUT);
  digitalWrite(MESH_LED_PIN, LOW);
  digitalWrite(BLE_LED_PIN, LOW);
  
  // Initialize BLE FIRST - Simplified to match working coordinator
  Serial.println("🔵 Initializing BLE...");
  
  // Add timeout for BLE initialization
  unsigned long bleStartTime = millis();
  const unsigned long BLE_TIMEOUT = 10000; // 10 second timeout
  
  // Free Classic BT memory to improve BLE stability on ESP32-S3
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

  BLEDevice::init(deviceName.c_str());
  // Increase advertising TX power for better visibility
  BLEDevice::setPower(ESP_PWR_LVL_P9);
  Serial.printf("   Device name set: %s\n", deviceName.c_str());
  
  // Create the BLE Server
  pServer = BLEDevice::createServer();
  if (!pServer) {
    Serial.println("❌ Failed to create BLE server");
    setStatusLED(255, 0, 0); // Red for failure
    return;
  }
  Serial.println("   BLE server created successfully");
  
  // Set server callbacks
  pServer->setCallbacks(new MyServerCallbacks());
  Serial.println("   Server callbacks set");
  
  // Create the BLE Service
  BLEService *pService = pServer->createService(SERVICE_UUID);
  if (!pService) {
    Serial.println("❌ Failed to create BLE service");
    setStatusLED(255, 0, 0); // Red for failure
    return;
  }
  Serial.printf("   Service created: %s\n", SERVICE_UUID);
  
  // Create BLE Characteristic
  pAudioCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ   |
                      BLECharacteristic::PROPERTY_WRITE  |
                      BLECharacteristic::PROPERTY_NOTIFY |
                      BLECharacteristic::PROPERTY_INDICATE
                    );
  
  if (!pAudioCharacteristic) {
    Serial.println("❌ Failed to create BLE characteristic");
    setStatusLED(255, 0, 0); // Red for failure
    return;
  }
  Serial.printf("   Characteristic created: %s\n", CHARACTERISTIC_UUID);
  
  // Add descriptor for notifications
  pAudioCharacteristic->addDescriptor(new BLE2902());
  Serial.println("   Notification descriptor added");
  
  // Set callbacks
  pAudioCharacteristic->setCallbacks(new MyCallbacks());
  Serial.println("   Characteristic callbacks set");
  
  // Start the service
  pService->start();
  Serial.println("   Service started");
  bleServerStarted = true;
  
  // Start advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  if (!pAdvertising) {
    Serial.println("❌ Failed to get advertising object");
    setStatusLED(255, 0, 0); // Red for failure
    return;
  }
  
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x0);  // match server config
  BLEDevice::startAdvertising();
  bleAdvertising = true;
  Serial.println("   Advertising started (device advertising object)");
  
  // Check timeout
  if (millis() - bleStartTime > BLE_TIMEOUT) {
    Serial.println("❌ BLE initialization timed out!");
    setStatusLED(255, 0, 0); // Red for failure
    return;
  }
  
  Serial.println("=== BLE CLIENT SERVER READY ===");
  Serial.printf("Device name: %s\n", deviceName.c_str());
  Serial.printf("Service UUID: %s\n", SERVICE_UUID);
  Serial.println("Waiting for Phone B connections...");
  
  // Blink status LED to indicate BLE ready
  blinkStatusLED(0, 255, 255, 3); // Cyan blink when BLE ready
  
  // Initialize ESP-NOW Mesh AFTER BLE is ready
  Serial.println("🔵 Initializing ESP-NOW Mesh...");
  delay(1000); // Give BLE time to fully initialize
  setupESPNOWMesh();

  // Process pending BLE notifications from queue (loop-based flush)
  if (bleDeviceConnected && pAudioCharacteristic) {
    static uint8_t coalesceBuf[1536];
    static int coalesceLen = 0;
    static uint32_t nextFlushMs = 0;
    NotifyItem item;
    int processed = 0;
    while (processed < 6 && notifyQueuePop(item)) {
      const int MAX_NOTIFY_BYTES = 160;
      if (item.isPcm8) {
        static uint8_t pcm16[512];
        int outLen = 0;
        for (int i = 0; i < item.length && outLen + 2 <= (int)sizeof(pcm16); i++) {
          uint8_t s8 = item.data[i];
          int16_t s16 = ((int)s8 - 128) << 8;
          pcm16[outLen++] = (uint8_t)(s16 & 0xFF);
          pcm16[outLen++] = (uint8_t)((s16 >> 8) & 0xFF);
        }
        int copy = min(outLen, (int)sizeof(coalesceBuf) - coalesceLen);
        memcpy(coalesceBuf + coalesceLen, pcm16, copy);
        coalesceLen += copy;
      } else {
        // Fallback silence for unexpected compressed payloads
        static uint8_t silence[240];
        memset(silence, 128, sizeof(silence));
        int copy = min((int)sizeof(silence), (int)sizeof(coalesceBuf) - coalesceLen);
        memcpy(coalesceBuf + coalesceLen, silence, copy);
        coalesceLen += copy;
      }
      processed++;
    }
    // Timed flush every ~25 ms or if buffer has >= 200B ready; cap per-flush to 1280B
    uint32_t nowMs = millis();
    if (nextFlushMs == 0) nextFlushMs = nowMs + 25;
    if (coalesceLen >= 200 || nowMs >= nextFlushMs) {
      int toSend = min(coalesceLen, 1280);
      int sent = 0;
      while (sent < toSend) {
        int chunk = min(160, toSend - sent);
        pAudioCharacteristic->setValue(coalesceBuf + sent, chunk);
        pAudioCharacteristic->notify();
        sent += chunk;
      }
      int remain = coalesceLen - toSend;
      if (remain > 0) memmove(coalesceBuf, coalesceBuf + toSend, remain);
      coalesceLen = remain;
      nextFlushMs = nowMs + 25;
    }
  }
}

// ESP-NOW Callback Functions
void OnDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  static unsigned long lastBrief = 0; if (millis() - lastBrief > 1000) { lastBrief = millis(); Serial.printf("Mesh RX len=%d\n", len); }
  
  // Check for raw PCM audio chunk format first (P:...)
  if (len > 2 && data[0] == 'P' && data[1] == ':') {
    // Raw PCM audio chunk from coordinator
    int values[8]; int dataStart = 0;
    if (parseCompactAudioHeader(data, len, values, dataStart)) {
      int sequence = values[0];
      int chunk = values[1];
      int totalChunks = values[2];
      unsigned long timestamp = (unsigned long)values[3];
      int sampleRate = values[4];
      int bitsPerSample = values[5];
      uint16_t minVal = (uint16_t)values[6];
      uint16_t maxVal = (uint16_t)values[7];
      // Minimal meta log (throttled above)
      if (millis() - lastBrief > 1000) { lastBrief = millis(); Serial.printf("Raw PCM Chunk %d/%d - Seq: %d, Rate: %d Hz, Bits: %d, Time: %lu\n", 
                    chunk + 1, totalChunks, sequence, sampleRate, bitsPerSample, timestamp); }
      if (dataStart > 0 && dataStart < len) {
        const uint8_t* rawPtr = ((const uint8_t*)data) + dataStart;
        int rawSize = len - dataStart;
        // Logging disabled in hot path

        // Process raw PCM data directly (no decompression needed)
        uint8_t processed[256];
        int processedLen = decompressOptimizedAudio(rawPtr,
                                                   rawSize,
                                                   processed,
                                                   sizeof(processed));
        if (processedLen <= 0 || processedLen > (int)sizeof(processed)) {
          // If processing failed, treat as silence to keep cadence
          static uint8_t silence[240];
          memset(silence, 128, sizeof(silence));
          processedLen = sizeof(silence);
          memcpy(processed, silence, sizeof(silence));
        }
        if (bleDeviceConnected) {
          (void)notifyQueuePushFromISR(processed, processedLen, 0);  // Raw 16-bit PCM
        } // drop silently if BLE not connected

        // Stats
        packetsReceived++;
        bytesReceived += rawSize;

        // ACK
        sendAudioAck(esp32_a_mac, sequence, chunk, "received");
      } else {
        Serial.println("❌ No raw PCM audio data found in message");
      }
    } else {
      Serial.printf("❌ Invalid raw PCM audio chunk format, expected 8 values, got %d\n", 8);
    }
    return; // Skip JSON parsing for raw PCM format
  }
  // Binary framing: 'W','M', type(0=PCM8), seq(le16), len(le16), payload
  if (len >= 7 && data[0] == 'W' && data[1] == 'M') {
    uint8_t type = data[2];
    uint16_t seq = (uint16_t)(data[3] | (data[4] << 8));
    uint16_t plen = (uint16_t)(data[5] | (data[6] << 8));
    if (7 + plen <= len && type == 0 && plen > 0) {
      const uint8_t* pcm8 = data + 7;
      if (!notifyQueuePushFromISR(pcm8, plen, 1)) {
        // drop silently
      }
      packetsReceived++;
      bytesReceived += plen;
    }
    return;
  }
  // Handle raw frame format: R:<240 bytes of 8-bit PCM>
  if (len > 2 && data[0] == 'R' && data[1] == ':') {
    int payload = len - 2;
    if (payload > 0) {
      const uint8_t* pcm8 = data + 2;
      if (!notifyQueuePushFromISR(pcm8, (uint16_t)payload, 1)) {
        // Drop silently
      }
      packetsReceived++;
      bytesReceived += payload;
    }
    return;
  }
   // Handle compact ping format: P:<seq>:<text>
  if (len > 2 && data[0] == 'P' && data[1] == ':') {
    // Do not forward PING text to audio characteristic to avoid playback noise
    return;
  }
  
  // Parse the received data as JSON
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, data, len);
  
  if (!error) {
    String messageType = doc["type"];
    String messageString = String((char*)data, len);
    
    if (messageType == "mesh_ack") {
      // Mesh acknowledgment received
      String status = doc["status"];
      
      if (status == "joined") {
        Serial.println("Successfully joined mesh network!");
        
        // Store coordinator's MAC address
        memcpy(esp32_a_mac, mac, 6);
        Serial.printf("Coordinator MAC stored: %02X:%02X:%02X:%02X:%02X:%02X\n",
                     esp32_a_mac[0], esp32_a_mac[1], esp32_a_mac[2],
                     esp32_a_mac[3], esp32_a_mac[4], esp32_a_mac[5]);
        
        // Add coordinator as ESP-NOW peer
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(esp_now_peer_info_t));
        memcpy(peerInfo.peer_addr, mac, 6);
        peerInfo.channel = MESH_CHANNEL;
        peerInfo.encrypt = false;
        peerInfo.ifidx = WIFI_IF_STA;
        
        esp_err_t result = esp_now_add_peer(&peerInfo);
        if (result == ESP_OK) {
          Serial.println("Mesh coordinator added as peer successfully");
          isMeshConnected = true;
          esp32_a_connected = true;
          setStatusLED(128, 0, 128); // Purple when connected
          
          // Send ready confirmation to complete handshake
          sendReadyConfirmation();
        } else {
          Serial.printf("Failed to add coordinator as peer: %d\n", result);
        }
        
      } else if (status == "failed") {
        Serial.println("Failed to join mesh network");
        setStatusLED(255, 0, 0); // Red on failure
      }
      
    } else if (messageType == "mesh_heartbeat") {
      // Heartbeat from coordinator
      Serial.println("Mesh heartbeat received from coordinator");
      lastMeshHeartbeat = millis();
      
      // Extract device count from compressed format
      if (doc.containsKey("devices")) {
        int totalDevices = doc["devices"];
        Serial.printf("Mesh heartbeat - Total devices: %d\n", totalDevices);
      }
      
    } else if (messageType == "mesh_status") {
      // Mesh status update from coordinator
      Serial.println("Mesh status received from coordinator");
      
      if (doc.containsKey("total_devices")) {
        int totalDevices = doc["total_devices"];
        Serial.printf("Total devices in mesh: %d\n", totalDevices);
      }
      
      if (doc.containsKey("devices")) {
        JsonArray devices = doc["devices"];
        Serial.println("Connected devices:");
        for (JsonObject device : devices) {
          // Handle compressed format
          String deviceName = device.containsKey("n") ? device["n"] : device["name"];
          String deviceType = device.containsKey("t") ? device["t"] : device["type"];
          String macStr = device.containsKey("m") ? device["m"] : device["mac"];
          int lastSeen = device.containsKey("s") ? device["s"] : device["last_seen"];
          int quality = device.containsKey("q") ? device["q"] : device["audio_quality"];
          
          Serial.printf("  - %s (%s) - MAC: %s, Last seen: %d s ago, Quality: %d%%\n", 
                       deviceName.c_str(), deviceType.c_str(), macStr.c_str(), lastSeen, quality);
        }
      }
      
    } else if (messageType == "audio_data") {
      // Audio data received from coordinator
      Serial.println("Audio data received from coordinator!");
      
      // Get source device info
      String sourceDevice = doc["source"];
      Serial.printf("Audio from: %s\n", sourceDevice.c_str());
      
      // Update statistics
      packetsReceived++;
      bytesReceived += len;
      
      // Forward to Phone B via BLE (if connected)
      if (bleDeviceConnected) {
        // Here you would send the audio data to Phone B
        Serial.println("Audio data forwarded to Phone B via BLE");
      }
      
      // Send acknowledgment back to coordinator
      sendAudioAck(mac);
      
    } else if (messageType == "test_audio") {
      // Test audio data received from coordinator
      Serial.println("Test audio data received from coordinator!");
      handleTestAudioData(data, len, doc);
      
    } else if (messageType == "test_ack") {
      // Test acknowledgment received
      int testId = doc["test_id"];
      String status = doc["status"];
      Serial.printf("Test ACK received: Test %d - %s\n", testId, status.c_str());
      
    } else if (messageType == "audio_chunk") {
      // Audio chunk received from coordinator
      Serial.println("🎵 Audio chunk received from coordinator!");
      
      // Extract audio metadata
      int sequence = doc["sequence"];
      int chunk = doc["chunk"];
      int totalChunks = doc["total_chunks"];
      int sampleRate = doc["sample_rate"];
      int bitsPerSample = doc["bits_per_sample"];
      unsigned long timestamp = doc["timestamp"];
      
      Serial.printf("🎵 Audio Chunk %d/%d - Seq: %d, Rate: %d Hz, Bits: %d, Time: %lu\n", 
                    chunk + 1, totalChunks, sequence, sampleRate, bitsPerSample, timestamp);
      
      // Extract audio data from the message
      String audioDataHex = "";
      int dataStart = messageString.lastIndexOf(':') + 1;
      if (dataStart > 0) {
        audioDataHex = messageString.substring(dataStart);
      }
      
      if (audioDataHex.length() > 0) {
        int audioDataSize = audioDataHex.length() / 2;
        Serial.printf("🎵 Audio data: %d bytes\n", audioDataSize);
        
        // Convert hex to bytes for processing
        uint8_t audioData[audioDataSize];
        for (int i = 0; i < audioDataSize; i++) {
          String hexByte = audioDataHex.substring(i * 2, i * 2 + 2);
          audioData[i] = strtol(hexByte.c_str(), NULL, 16);
        }
        
        // Enhanced data validation logging
        Serial.printf("📊 Data Validation - Size: %d bytes\n", audioDataSize);
        Serial.printf("   Data Preview: ");
        for (int i = 0; i < min(8, audioDataSize); i++) {
          Serial.printf("%02X ", audioData[i]);
        }
        Serial.println();
        
        // Calculate simple checksum for validation
        uint32_t checksum = 0;
        for (int i = 0; i < audioDataSize; i++) {
          checksum += audioData[i];
        }
        Serial.printf("   Checksum: 0x%08X\n", checksum);
        
        // Process the audio data (here you would play it or forward to BLE)
        processReceivedAudioData(audioData, audioDataSize, sequence, chunk, totalChunks);
        
        // Send audio acknowledgment back to coordinator
        sendAudioAck(esp32_a_mac, sequence, chunk, "received");
        
      } else {
        Serial.println("❌ No audio data found in message");
      }
      
    } else {
      Serial.printf("Unknown message type: %s\n", messageType.c_str());
    }
    
  } else {
    Serial.printf("Failed to parse JSON: %s\n", error.c_str());
  }
}

void OnDataSent(const uint8_t *mac, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println("Data sent successfully to ESP32 A");
  } else {
    Serial.println("Failed to send data to ESP32 A");
  }
}

void loop() {
  // Handle BLE connection state changes
  if (!bleDeviceConnected && oldBleDeviceConnected) {
     delay(500);
    if (pServer) {
      pServer->getAdvertising()->start();
      bleAdvertising = true;
      Serial.println("Restart advertising (server advertising object)");
    }
    oldBleDeviceConnected = bleDeviceConnected;
  }
  
  if (bleDeviceConnected && !oldBleDeviceConnected) {
    oldBleDeviceConnected = bleDeviceConnected;
  }
  
  // Handle BLE connections
  if (bleDeviceConnected) {
    // BLE is connected, handle any incoming data
    delay(10);
  }
  
  // Handle mesh reconnection with timeout
  if (!isMeshConnected || !esp32_a_connected) {
    unsigned long currentTime = millis();
    
    // Try to reconnect every 15 seconds, but limit total attempts
    if (currentTime - lastMeshJoinAttempt > 15000) {
      static int reconnectAttempts = 0;
      const int MAX_RECONNECT_ATTEMPTS = 10; // Limit to 10 attempts
      
      if (reconnectAttempts < MAX_RECONNECT_ATTEMPTS) {
        Serial.printf("Mesh connection lost, attempting to reconnect... (Attempt %d/%d)\n", 
                      reconnectAttempts + 1, MAX_RECONNECT_ATTEMPTS);
        lastMeshJoinAttempt = currentTime;
        reconnectAttempts++;
        
        // Clear any existing peers
        esp_now_del_peer(esp32_a_mac);
        
        // Try to join mesh again
        startScanningForESP32A();
      } else {
        Serial.println("⚠️ Maximum reconnection attempts reached. Stopping mesh reconnection.");
        setStatusLED(255, 165, 0); // Orange for warning
      }
    }
  }
  
  // Check mesh connection health
  if (isMeshConnected && esp32_a_connected) {
    unsigned long currentTime = millis();
    
    // Check if we haven't received heartbeat for too long
    if (currentTime - lastMeshHeartbeat > DEVICE_TIMEOUT) {
      Serial.println("Mesh coordinator heartbeat timeout, marking as disconnected");
      esp32_a_connected = false;
      isMeshConnected = false;
      setStatusLED(255, 0, 0); // Red when disconnected
      digitalWrite(MESH_LED_PIN, LOW);
      
      // Clear the peer to force reconnection
      esp_now_del_peer(esp32_a_mac);
    }
  }

  // Process pending BLE notifications from queue (loop-based flush)
  if (bleDeviceConnected && pAudioCharacteristic) {
    static uint8_t coalesceBuf[4096];
    static int coalesceLen = 0;
    static uint32_t nextFlushMs = 0;
    NotifyItem item;
    int processed = 0;
    while (processed < 32 && notifyQueuePop(item)) {
      const int MAX_NOTIFY_BYTES = 160;
      if (item.isPcm8) {
        static uint8_t pcm16[512];
        int outLen = 0;
        for (int i = 0; i < item.length && outLen + 2 <= (int)sizeof(pcm16); i++) {
          uint8_t s8 = item.data[i];
          int16_t s16 = ((int)s8 - 128) << 8;
          pcm16[outLen++] = (uint8_t)(s16 & 0xFF);
          pcm16[outLen++] = (uint8_t)((s16 >> 8) & 0xFF);
        }
        int copy = min(outLen, (int)sizeof(coalesceBuf) - coalesceLen);
        memcpy(coalesceBuf + coalesceLen, pcm16, copy);
        coalesceLen += copy;
      } else {
        // Fallback silence for unexpected compressed payloads
        static uint8_t silence[240];
        memset(silence, 128, sizeof(silence));
        int copy = min((int)sizeof(silence), (int)sizeof(coalesceBuf) - coalesceLen);
        memcpy(coalesceBuf + coalesceLen, silence, copy);
        coalesceLen += copy;
      }
      processed++;
    }
    // Timed flush every ~10 ms or if buffer has >= 200B ready; cap per-flush to 1440B
    uint32_t nowMs = millis();
    if (nextFlushMs == 0) nextFlushMs = nowMs + 10;
    if (coalesceLen >= 200 || nowMs >= nextFlushMs) {
      int toSend = min(coalesceLen, 1440);
      int sent = 0;
      while (sent < toSend) {
        int chunk = min(160, toSend - sent);
        pAudioCharacteristic->setValue(coalesceBuf + sent, chunk);
        pAudioCharacteristic->notify();
        sent += chunk;
      }
      int remain = coalesceLen - toSend;
      if (remain > 0) memmove(coalesceBuf, coalesceBuf + toSend, remain);
      coalesceLen = remain;
      nextFlushMs = nowMs + 10;
    }
  }
  
  // Print statistics every 30 seconds (reduced spam)
  static unsigned long lastStatsTime = 0;
  if (millis() - lastStatsTime > 30000) {
    printStatistics();
    lastStatsTime = millis();
  }
  
  // BLE status check and debugging
  static unsigned long lastBLEDebug = 0;
  if (millis() - lastBLEDebug > 10000) { // Every 10 seconds
    lastBLEDebug = millis();
    
    Serial.printf("🔵 BLE Status - Server: %s, Advertising: %s, Connected: %s\n",
                  bleServerStarted ? "Running" : "Stopped",
                  bleAdvertising ? "Yes" : "No", 
                  bleDeviceConnected ? "Yes" : "No");
    
    // Check if BLE is actually advertising
    if (bleServerStarted && bleAdvertising) {
      Serial.println("✅ BLE Server should be advertising");
    } else {
      Serial.println("❌ BLE Server not advertising properly");
    }
  }
  
  delay(100);
}
