/*
 * ESP32-S3 BLE Audio Server (Arduino C++ Version)
 * 
 * This ESP32 acts as a BLE server that receives audio data from an Android phone
 * and can forward it to another ESP32 device (for the mesh network).
 * 
 * UUIDs match the Android app:
 * - Service UUID: 4fafc201-1fb5-459e-8fcc-c5c9c331914b
 * - Characteristic UUID: beb5483e-36e1-4688-b7f5-ea07361b26a8
 * - Descriptor UUID: 00002902-0000-1000-8000-00805f9b34fb
 * 
 * Compile with: PlatformIO + Arduino framework
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_bt.h>
#include <esp_wifi.h>
#include <esp_gap_ble_api.h>
#include <WiFi.h>
#include <esp_now.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>

// BLE UUIDs matching the Android app
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define DESCRIPTOR_UUID     "00002902-0000-1000-8000-00805f9b34fb"

// Device name that Android app looks for
#define DEVICE_NAME "ESP32S3_Audio_Server"

// Audio buffer size (320 bytes as expected by Android app)
#define AUDIO_BUFFER_SIZE 1024  // Increased to handle multiple chunks safely

// Pin definitions for ESP32-S3-DevKitC-1
#define STATUS_LED_PIN 48    // GPIO48 - RGB LED (built-in)
#define CONNECTION_LED_PIN 2 // GPIO2 - Available GPIO pin
#define NUM_LEDS 1           // Number of RGB LEDs

// Mesh Network Configuration for 4 Devices
#define MESH_CHANNEL 1
#define MAX_MESH_DEVICES 4
#define MESH_BROADCAST_INTERVAL 5000  // 5 seconds
#define MESH_HEARTBEAT_INTERVAL 5000  // 5 seconds (increased for stability)
#define DEVICE_TIMEOUT 30000          // 30 seconds (increased for stability)

// Mesh device management
struct MeshDevice {
  uint8_t mac[6];
  String deviceName;
  String deviceType;
  unsigned long lastSeen;
  bool isActive;
  bool isCoordinator;
  int audioQuality;
};

MeshDevice meshDevices[MAX_MESH_DEVICES];
int meshDeviceCount = 0;
bool isMeshCoordinator = true;

// Mesh network state
bool meshNetworkActive = false;
unsigned long lastMeshBroadcast = 0;
unsigned long lastMeshHeartbeat = 0;
unsigned long lastDeviceCleanup = 0;

// Audio streaming
bool isAudioStreaming = false;
unsigned long lastAudioStats = 0;

// ESP32 B's MAC address (will be updated when it connects)
uint8_t esp32_b_mac[6] = {0};
bool esp32_b_connected = false;

// Global variables
BLEServer* pServer = NULL;
BLECharacteristic* pAudioCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// Neopixel LED control
Adafruit_NeoPixel pixels(NUM_LEDS, STATUS_LED_PIN, NEO_GRB + NEO_KHZ800);

// Audio buffer
uint8_t audioBuffer[AUDIO_BUFFER_SIZE];
size_t audioBufferSize = 0;

// Statistics
unsigned long packetsReceived = 0;
unsigned long bytesReceived = 0;
unsigned long lastStatsTime = 0;

// Forward declarations
void setStatusLED(uint8_t r, uint8_t g, uint8_t b);
void blinkStatusLED(uint8_t r, uint8_t g, uint8_t b, int times);
void processAudioData(uint8_t* data, size_t length);
void analyzeAudioData(uint8_t* data, size_t length);
void setupESPNOWMesh();
void OnDataRecv(const uint8_t *mac, const uint8_t *data, int len);
bool addDeviceToMesh(const uint8_t* mac, const String& deviceName, const String& deviceType);
bool removeDeviceFromMesh(const uint8_t* mac);
void updateDeviceHeartbeat(const uint8_t* mac);
void cleanupInactiveDevices();
void updateMeshStatusLED();
void relayAudioToMesh(const uint8_t* sourceMac, const uint8_t* data, int len);
void sendMeshAck(const uint8_t* mac, const String& status);
void sendAudioAck(const uint8_t* mac);
void sendMeshHeartbeat();
void broadcastMeshStatus();
void handleTestCommand(const String& command);
void sendTestAck(const uint8_t* mac, int testId, const String& status);
void startAudioStream();
void stopAudioStream();
void addAudioData(const uint8_t* data, int length);
void sendAudioChunks();
int compressAudioData(const uint8_t* input, int inputLength, uint8_t* output);
void calculateAudioStats(const uint8_t* data, int length, uint16_t& minVal, uint16_t& maxVal, uint32_t& avgVal);
// Forward decls for BLE write queue helpers
struct IncomingBleItem;
static inline bool bleInPushFromISR(const uint8_t* buf, uint16_t len);
static inline bool bleInPop(IncomingBleItem &out);
void sendBeepOnce();
volatile bool pendingBeep = false;

// FreeRTOS task handle for the audio sender
TaskHandle_t AudioSenderTaskHandle = NULL;

// Dedicated task for sending audio data over BLE
void AudioSenderTask(void *pvParameters) {
  const TickType_t xFrequency = pdMS_TO_TICKS(6); // Roughly 6.25ms
  TickType_t xLastWakeTime = xTaskGetTickCount();

  for (;;) {
    // Wait for the next cycle.
    vTaskDelayUntil(&xLastWakeTime, xFrequency);

    if (isAudioStreaming && (deviceConnected || meshDeviceCount > 0)) {
      sendAudioChunks();
    }
  }
}

// Core Mesh Management Functions
bool addDeviceToMesh(const uint8_t* mac, const String& deviceName, const String& deviceType) {
  // Check if device already exists
  for (int i = 0; i < meshDeviceCount; i++) {
    if (memcmp(meshDevices[i].mac, mac, 6) == 0) {
      // Update existing device
      meshDevices[i].lastSeen = millis();
      // Don't mark as active until ready confirmation
      Serial.printf("Updated existing device: %s\n", deviceName.c_str());
      return true;
    }
  }
  
  // Add new device if we have space
  if (meshDeviceCount < MAX_MESH_DEVICES) {
    memcpy(meshDevices[meshDeviceCount].mac, mac, 6);
    meshDevices[meshDeviceCount].deviceName = deviceName;
    meshDevices[meshDeviceCount].deviceType = deviceType;
    meshDevices[meshDeviceCount].lastSeen = millis();
    meshDevices[meshDeviceCount].isActive = false;  // Not active until ready
    meshDevices[meshDeviceCount].isCoordinator = false;
    meshDevices[meshDeviceCount].audioQuality = 100;
    
    // Add as ESP-NOW peer with proper configuration
    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = MESH_CHANNEL;  // Use the same channel
    peerInfo.encrypt = false;         // No encryption for now
    peerInfo.ifidx = WIFI_IF_STA;    // Use WiFi STA interface
    
    esp_err_t result = esp_now_add_peer(&peerInfo);
    if (result == ESP_OK) {
      Serial.printf("Added new device to mesh: %s (Total: %d)\n", deviceName.c_str(), meshDeviceCount + 1);
      meshDeviceCount++;
      updateMeshStatusLED(); // Update LED status when device added
      return true;
    } else {
      Serial.printf("Failed to add peer %s: %d\n", deviceName.c_str(), result);
      return false;
    }
  }
  
  Serial.println("Maximum mesh devices reached");
  return false;
}

bool removeDeviceFromMesh(const uint8_t* mac) {
  for (int i = 0; i < meshDeviceCount; i++) {
    if (memcmp(meshDevices[i].mac, mac, 6) == 0) {
      // Remove ESP-NOW peer
      esp_now_del_peer(mac);
      
      // Shift remaining devices
      for (int j = i; j < meshDeviceCount - 1; j++) {
        meshDevices[j] = meshDevices[j + 1];
      }
      meshDeviceCount--;
      
      Serial.printf("Removed device from mesh: %s (Total: %d)\n", 
                   meshDevices[i].deviceName.c_str(), meshDeviceCount);
      updateMeshStatusLED(); // Update LED status when device removed
      return true;
    }
  }
  return false;
}

void updateDeviceHeartbeat(const uint8_t* mac) {
  for (int i = 0; i < meshDeviceCount; i++) {
    if (memcmp(meshDevices[i].mac, mac, 6) == 0) {
      meshDevices[i].lastSeen = millis();
      meshDevices[i].isActive = true;
            break;
    }
}
}

void cleanupInactiveDevices() {
  unsigned long currentTime = millis();
  
  // Safety check: ensure mesh is active
  if (!meshNetworkActive) {
    return;
  }
  
  for (int i = 0; i < meshDeviceCount; i++) {
    // Safety check: ensure device index is valid
    if (i >= MAX_MESH_DEVICES) {
      Serial.println("cleanupInactiveDevices: Device index out of bounds");
            break;
    }
    
    if (meshDevices[i].isActive && 
        (currentTime - meshDevices[i].lastSeen) > DEVICE_TIMEOUT) {
      
      Serial.printf("Device %s timed out, removing from mesh\n", 
                   meshDevices[i].deviceName.c_str());
      
      // Remove the device safely
      if (removeDeviceFromMesh(meshDevices[i].mac)) {
        i--; // Adjust index after removal
            } else {
        Serial.printf("Failed to remove timed out device %s\n", 
                     meshDevices[i].deviceName.c_str());
      }
    }
  }
}

void updateMeshStatusLED() {
  if (meshDeviceCount == 0) {
    setStatusLED(255, 0, 0); // Red - no devices
  } else if (meshDeviceCount == 1) {
    setStatusLED(255, 165, 0); // Orange - 1 device
  } else if (meshDeviceCount == 2) {
    setStatusLED(255, 255, 0); // Yellow - 2 devices
  } else if (meshDeviceCount == 3) {
    setStatusLED(0, 255, 0); // Green - 3 devices
  } else {
    setStatusLED(0, 0, 255); // Blue - 4 devices (full)
  }
}

void processAudioData(uint8_t* data, size_t length) {
  // DEPRECATED: This function is no longer used
  // Audio now flows through: BLE → addAudioData() → sendAudioChunks() → P: format
  Serial.println("⚠️ processAudioData() called - this should not happen with new audio flow");
}

void analyzeAudioData(uint8_t* data, size_t length) {
    uint32_t sum = 0;
    uint8_t min_val = 255;
    uint8_t max_val = 0;
    
    for (size_t i = 0; i < length; i++) {
        sum += data[i];
        if (data[i] < min_val) min_val = data[i];
        if (data[i] > max_val) max_val = data[i];
    }
    
    uint8_t avg_val = sum / length;
    
  Serial.printf("Audio stats - Min: %d, Max: %d, Avg: %d\n", min_val, max_val, avg_val);
}

void printStatistics() {
  Serial.println("=== MESH NETWORK STATISTICS ===");
  Serial.printf("Connection status: %s\n", deviceConnected ? "Connected" : "Disconnected");
  Serial.printf("Mesh devices: %d/%d\n", meshDeviceCount, MAX_MESH_DEVICES);
  Serial.printf("Mesh network: %s\n", meshNetworkActive ? "Active" : "Inactive");
  Serial.printf("Audio streaming: %s\n", isAudioStreaming ? "Yes" : "No");
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  
  if (meshDeviceCount > 0) {
    Serial.println("--- Connected Devices ---");
    for (int i = 0; i < meshDeviceCount; i++) {
      if (meshDevices[i].isActive) {
        char macStr[18];
        sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
                meshDevices[i].mac[0], meshDevices[i].mac[1], meshDevices[i].mac[2],
                meshDevices[i].mac[3], meshDevices[i].mac[4], meshDevices[i].mac[5]);
        
        Serial.printf("  %d. %s (%s) - MAC: %s\n", 
                     i + 1, 
                     meshDevices[i].deviceName.c_str(),
                     meshDevices[i].deviceType.c_str(),
                     macStr);
        
        unsigned long timeSinceLastSeen = millis() - meshDevices[i].lastSeen;
        Serial.printf("      Last seen: %lu ms ago, Quality: %d%%\n", 
                     timeSinceLastSeen, meshDevices[i].audioQuality);
      }
    }
  }
  
  Serial.println("================================");
}

void sendKeepAlive() {
  if (deviceConnected && pAudioCharacteristic != nullptr) {
    // Send a small keep-alive packet
    uint8_t keepAliveData[] = {0xAA, 0x55, 0xAA, 0x55};
    pAudioCharacteristic->setValue(keepAliveData, 4);
    pAudioCharacteristic->notify();
    Serial.println("Keep-alive sent");
  }
}

// ESP-NOW Mesh Functions
void setupESPNOWMesh() {
  Serial.println("Setting up Multi-Device ESP-NOW Mesh Network...");
  
  // Completely disable WiFi AP mode - only use STA for ESP-NOW
  WiFi.mode(WIFI_STA);
  // Enable WiFi modem sleep for BLE/WiFi coexistence
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
  
  // Register callback
  esp_now_register_recv_cb(OnDataRecv);
  
  // Initialize mesh device array
  for (int i = 0; i < MAX_MESH_DEVICES; i++) {
    meshDevices[i].isActive = false;
    meshDevices[i].lastSeen = 0;
  }
  
  meshNetworkActive = true;
  
  Serial.println("Multi-Device ESP-NOW Mesh initialized successfully");
  Serial.println("Waiting for devices to join...");
  
  // Show mesh ready with purple LED
  setStatusLED(255, 0, 255);
  delay(500);
}

void OnDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  Serial.println("=== MESH DATA RECEIVED ===");
  Serial.printf("From MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", 
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("Data length: %d\n", len);
  
  // Parse the received data
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, data, len);
  
  if (!error) {
    String messageType = doc["type"];
    
    if (messageType == "mesh_join") {
      // New device requesting to join mesh network
      String deviceName = doc["device_name"];
      String deviceType = doc["device_type"];
      
      Serial.println("New device requesting to join mesh network!");
      Serial.printf("Device: %s (%s)\n", deviceName.c_str(), deviceType.c_str());
      
      // Add device to mesh
      if (addDeviceToMesh(mac, deviceName, deviceType)) {
        Serial.println("Device added to mesh successfully");
        
        // Send acknowledgment - device will respond with ready confirmation
        sendMeshAck(mac, "joined");
      } else {
        Serial.println("Failed to add device to mesh");
        sendMeshAck(mac, "failed");
      }
      
    } else if (messageType == "mesh_ready") {
      // Device confirms it's ready for communication
      String deviceName = doc["source"];
      Serial.printf("Device %s confirmed ready for communication\n", deviceName.c_str());
      
      // Mark device as ready for communication
      for (int i = 0; i < meshDeviceCount; i++) {
        if (memcmp(meshDevices[i].mac, mac, 6) == 0) {
          meshDevices[i].lastSeen = millis();
          meshDevices[i].isActive = true;
          Serial.printf("Device %s marked as ready\n", deviceName.c_str());
          break;
        }
      }
      
    } else if (messageType == "audio_data") {
      Serial.println("Audio data received from mesh device!");
      
      // Get source device info
      String sourceDevice = doc["source"];
      Serial.printf("Audio from: %s\n", sourceDevice.c_str());
      
      // Forward audio to all other mesh devices (mesh relay)
      relayAudioToMesh(mac, data, len);
      
      // Send acknowledgment back to source
      sendAudioAck(mac);
      
    } else if (messageType == "mesh_heartbeat") {
      // Update device last seen time
      updateDeviceHeartbeat(mac);
      
    } else if (messageType == "mesh_leave") {
      Serial.println("Device leaving mesh network");
      removeDeviceFromMesh(mac);
      updateMeshStatusLED();
      
    } else if (messageType == "audio_ack") {
      // Audio was received by destination
      Serial.println("Audio acknowledgment received");
    }
  } else {
    Serial.println("Failed to parse mesh JSON message");
  }
}

void broadcastToMesh(const uint8_t* data, size_t length) {
  // This function is no longer needed as ESP32 B is removed.
  // The audio data is now directly forwarded to mesh devices.
}

void sendMeshBroadcast() {
  if (millis() - lastMeshBroadcast > MESH_BROADCAST_INTERVAL) {
    DynamicJsonDocument doc(256);
    doc["type"] = "mesh_heartbeat";
    doc["source"] = "ESP32_A_Server"; // Indicate this is from the server
    doc["timestamp"] = millis();
    doc["free_heap"] = ESP.getFreeHeap();
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    // Send heartbeat to all mesh devices
    for (int i = 0; i < meshDeviceCount; i++) {
      if (meshDevices[i].isActive) {
        esp_err_t result = esp_now_send(meshDevices[i].mac, 
                                       (uint8_t*)jsonString.c_str(), 
                                       jsonString.length());
        if (result != ESP_OK) {
          Serial.printf("Failed to send heartbeat to %s: %d\n", 
                       meshDevices[i].deviceName.c_str(), result);
        }
      }
    }
    
    lastMeshBroadcast = millis();
  }
}

// Audio Broadcasting and Mesh Communication Functions
void relayAudioToMesh(const uint8_t* sourceMac, const uint8_t* data, int len) {
  // Safety check: ensure we have valid data and mesh is active
  if (!meshNetworkActive || data == nullptr || len <= 0) {
    Serial.println("relayAudioToMesh: Invalid parameters or mesh not active");
    return;
  }
  
  // Don't relay back to the source device
  for (int i = 0; i < meshDeviceCount; i++) {
    // Safety check: ensure device is valid
    if (i >= MAX_MESH_DEVICES) {
      Serial.println("relayAudioToMesh: Device index out of bounds");
      break;
    }
    
    if (meshDevices[i].isActive && 
        (sourceMac == nullptr || memcmp(meshDevices[i].mac, sourceMac, 6) != 0)) {
      
      // Verify the peer is still valid before sending
      esp_now_peer_info_t peerInfo;
      if (esp_now_get_peer(meshDevices[i].mac, &peerInfo) == ESP_OK) {
        
        // Create audio relay message
        DynamicJsonDocument doc(1024);
        doc["type"] = "audio_data";
        doc["source"] = "ESP32_A_Server";
        doc["timestamp"] = millis();
        doc["data_length"] = len;
        
        // Convert binary data to hex for JSON (with bounds checking)
        String dataHex = "";
        int previewLength = min(len, 64); // Limit to first 64 bytes
        for (int j = 0; j < previewLength; j++) {
          char hex[3];
          sprintf(hex, "%02X", data[j]);
          dataHex += hex;
        }
        doc["data_preview"] = dataHex;
        
        String jsonString;
        serializeJson(doc, jsonString);
        
        // Send to this mesh device
        esp_err_t result = esp_now_send(meshDevices[i].mac, 
                                       (uint8_t*)jsonString.c_str(), 
                                       jsonString.length());
        if (result == ESP_OK) {
          Serial.printf("Audio relayed to %s\n", meshDevices[i].deviceName.c_str());
        } else {
          Serial.printf("Failed to relay audio to %s: %d\n", 
                       meshDevices[i].deviceName.c_str(), result);
          
          // If sending fails, mark device as potentially disconnected
          if (result == ESP_ERR_ESPNOW_ARG || result == ESP_ERR_ESPNOW_NOT_FOUND) {
            Serial.printf("Peer validation failed for %s, removing from mesh\n", 
                         meshDevices[i].deviceName.c_str());
            removeDeviceFromMesh(meshDevices[i].mac);
            i--; // Adjust index after removal
          }
        }
      } else {
        Serial.printf("Peer validation failed for %s, removing from mesh\n", 
                     meshDevices[i].deviceName.c_str());
        removeDeviceFromMesh(meshDevices[i].mac);
        i--; // Adjust index after removal
      }
    }
  }
}

void sendMeshAck(const uint8_t* mac, const String& status) {
  DynamicJsonDocument ackDoc(256);
  ackDoc["type"] = "mesh_ack";
  ackDoc["source"] = "ESP32_A_Server";
  ackDoc["status"] = status;
  ackDoc["timestamp"] = millis();
  ackDoc["mesh_device_count"] = meshDeviceCount;
  
  String ackString;
  serializeJson(ackDoc, ackString);
  
  esp_err_t result = esp_now_send(mac, (uint8_t*)ackString.c_str(), ackString.length());
  if (result == ESP_OK) {
    Serial.printf("Mesh ACK sent to device: %s\n", status.c_str());
  } else {
    Serial.printf("Failed to send mesh ACK: %d\n", result);
  }
}

void sendAudioAck(const uint8_t* mac) {
  DynamicJsonDocument ackDoc(256);
  ackDoc["type"] = "audio_ack";
  ackDoc["source"] = "ESP32_A_Server";
  ackDoc["status"] = "received";
  ackDoc["timestamp"] = millis();
  
  String ackString;
  serializeJson(ackDoc, ackString);
  
  esp_err_t result = esp_now_send(mac, (uint8_t*)ackString.c_str(), ackString.length());
  if (result == ESP_OK) {
    Serial.println("Audio ACK sent");
  } else {
    Serial.printf("Failed to send audio ACK: %d\n", result);
  }
}

void sendMeshHeartbeat() {
  if (meshDeviceCount == 0) return;
  
  // Create minimal heartbeat message
  DynamicJsonDocument heartbeatDoc(128);  // Reduced from 256
  heartbeatDoc["type"] = "mesh_heartbeat";
  heartbeatDoc["source"] = "ESP32_A_Server";
  heartbeatDoc["timestamp"] = millis();
  heartbeatDoc["devices"] = meshDeviceCount;
  heartbeatDoc["mac"] = WiFi.macAddress();
  
  String heartbeatString;
  serializeJson(heartbeatDoc, heartbeatString);
  
  Serial.printf("Sending heartbeat to %d mesh devices (size: %d bytes)\n", meshDeviceCount, heartbeatString.length());
  
  // Send heartbeat to all mesh devices with proper validation
  for (int i = 0; i < meshDeviceCount; i++) {
    if (meshDevices[i].isActive) {
      Serial.printf("Attempting to send heartbeat to %s (MAC: %02X:%02X:%02X:%02X:%02X:%02X)\n",
                   meshDevices[i].deviceName.c_str(),
                   meshDevices[i].mac[0], meshDevices[i].mac[1], meshDevices[i].mac[2],
                   meshDevices[i].mac[3], meshDevices[i].mac[4], meshDevices[i].mac[5]);
      
      // Verify the peer is still valid before sending
      esp_now_peer_info_t peerInfo;
      esp_err_t peerResult = esp_now_get_peer(meshDevices[i].mac, &peerInfo);
      if (peerResult == ESP_OK) {
        Serial.printf("Peer validation successful for %s\n", meshDevices[i].deviceName.c_str());
        
        // Ensure message size is within limits
        if (heartbeatString.length() > 250) {
          Serial.printf("⚠️  Heartbeat message too large (%d bytes), truncating\n", heartbeatString.length());
          heartbeatString = heartbeatString.substring(0, 250);
        }
        
        esp_err_t result = esp_now_send(meshDevices[i].mac, 
                                       (uint8_t*)heartbeatString.c_str(), 
                                       heartbeatString.length());
        if (result == ESP_OK) {
          Serial.printf("Heartbeat sent successfully to %s\n", meshDevices[i].deviceName.c_str());
        } else {
          Serial.printf("Failed to send heartbeat to %s: %d (0x%04X)\n", 
                       meshDevices[i].deviceName.c_str(), result, result);
          
          // If sending fails, mark device as potentially disconnected
          if (result == ESP_ERR_ESPNOW_ARG || result == ESP_ERR_ESPNOW_NOT_FOUND) {
            Serial.printf("Peer validation failed for %s, removing from mesh\n", 
                         meshDevices[i].deviceName.c_str());
            removeDeviceFromMesh(meshDevices[i].mac);
            i--; // Adjust index after removal
          }
        }
      } else {
        Serial.printf("Peer validation failed for %s: %d (0x%04X), removing from mesh\n", 
                     meshDevices[i].deviceName.c_str(), peerResult, peerResult);
        removeDeviceFromMesh(meshDevices[i].mac);
        i--; // Adjust index after removal
      }
    }
  }
}

void broadcastMeshStatus() {
  if (meshDeviceCount == 0) return;
  
  // Create a minimal status message to stay within ESP-NOW limits
  DynamicJsonDocument statusDoc(256);  // Reduced from 512
  statusDoc["type"] = "mesh_status";
  statusDoc["source"] = "ESP32_A_Server";
  statusDoc["timestamp"] = millis();
  statusDoc["total_devices"] = meshDeviceCount;
  statusDoc["mesh_healthy"] = true;
  
  // Only include essential device info to keep message small
  if (meshDeviceCount > 0) {
    JsonArray devicesArray = statusDoc.createNestedArray("devices");
    for (int i = 0; i < meshDeviceCount && i < 2; i++) {  // Limit to 2 devices
      if (meshDevices[i].isActive) {
        JsonObject device = devicesArray.createNestedObject();
        // Use short MAC format to save space
        char macStr[13];  // Reduced from 18
        sprintf(macStr, "%02X%02X%02X%02X%02X%02X",
                meshDevices[i].mac[0], meshDevices[i].mac[1], meshDevices[i].mac[2],
                meshDevices[i].mac[3], meshDevices[i].mac[4], meshDevices[i].mac[5]);
        device["m"] = macStr;  // Short key
        device["n"] = meshDevices[i].deviceName;  // Short key
        device["t"] = meshDevices[i].deviceType;  // Short key
        device["s"] = (meshDevices[i].lastSeen / 1000);  // Seconds, not milliseconds
        device["q"] = meshDevices[i].audioQuality;  // Short key
      }
    }
  }
  
  String statusString;
  serializeJson(statusDoc, statusString);
  
  Serial.printf("📡 Status message size: %d bytes\n", statusString.length());
  
  // Broadcast to all mesh devices with proper error handling
  for (int i = 0; i < meshDeviceCount; i++) {
    if (meshDevices[i].isActive) {
      Serial.printf("Attempting to send status to %s (MAC: %02X:%02X:%02X:%02X:%02X:%02X)\n",
                   meshDevices[i].deviceName.c_str(),
                   meshDevices[i].mac[0], meshDevices[i].mac[1], meshDevices[i].mac[2],
                   meshDevices[i].mac[3], meshDevices[i].mac[4], meshDevices[i].mac[5]);
      
      // Verify the peer is still valid before sending
      esp_now_peer_info_t peerInfo;
      esp_err_t peerResult = esp_now_get_peer(meshDevices[i].mac, &peerInfo);
      if (peerResult == ESP_OK) {
        Serial.printf("Peer validation successful for %s\n", meshDevices[i].deviceName.c_str());
        
        // Check message size before sending
        if (statusString.length() > 250) {
          Serial.printf("⚠️  Status message too large (%d bytes), truncating\n", statusString.length());
          statusString = statusString.substring(0, 250);
        }
        
        esp_err_t result = esp_now_send(meshDevices[i].mac, 
                                       (uint8_t*)statusString.c_str(), 
                                       statusString.length());
        if (result == ESP_OK) {
          Serial.printf("Status sent successfully to %s\n", meshDevices[i].deviceName.c_str());
        } else {
          Serial.printf("Failed to send status to %s: %d (0x%04X)\n", 
                       meshDevices[i].deviceName.c_str(), result, result);
          
          // If sending fails, mark device as potentially disconnected
          if (result == ESP_ERR_ESPNOW_ARG || result == ESP_ERR_ESPNOW_NOT_FOUND) {
            Serial.printf("Peer validation failed for %s, removing from mesh\n", 
                         meshDevices[i].deviceName.c_str());
            removeDeviceFromMesh(meshDevices[i].mac);
            i--; // Adjust index after removal
          }
        }
      } else {
        Serial.printf("Peer validation failed for %s: %d (0x%04X), removing from mesh\n", 
                     meshDevices[i].deviceName.c_str(), peerResult, peerResult);
        removeDeviceFromMesh(meshDevices[i].mac);
        i--; // Adjust index after removal
      }
    }
  }
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

// Callback class for server events
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println("=== DEVICE CONNECTED ===");
      digitalWrite(CONNECTION_LED_PIN, HIGH);
      updateMeshStatusLED(); // Use mesh status instead of hardcoded green
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      Serial.println("=== DEVICE DISCONNECTED ===");
      digitalWrite(CONNECTION_LED_PIN, LOW);
      if (meshDeviceCount > 0) {
        setStatusLED(0, 255, 255); // Cyan - mesh active, BLE disconnected
      } else {
        setStatusLED(255, 0, 0); // Red - no connections at all
      }
    }
};

// Callback class for characteristic events
class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      std::string rxValue = pCharacteristic->getValue();
      
      if (rxValue.length() > 0) {
        // If this is a control request to emit a network beep, trigger it
        if (rxValue.rfind("BEEP", 0) == 0) { pendingBeep = true; return; }
        // Minimal work in BLE stack thread: enqueue and return
        if (!isAudioStreaming) {
          startAudioStream();
        }
        bleInPushFromISR((const uint8_t*)rxValue.data(), (uint16_t)rxValue.length());
      }
    }
    
    void onRead(BLECharacteristic *pCharacteristic) {
      Serial.println("Characteristic read request");
    }
};

// Callback class for characteristic events
class MyCharacteristicCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        size_t len = value.length();

        if (len > 0) {
            if (len == 4 && value == "BEEP") {
                Serial.println("Received 'BEEP' command, triggering test tone.");
                sendBeepOnce();
            } else {
                Serial.printf("ECHO RECV: len=%d, data=", len);
                for (int i=0; i<min((size_t)8, len); i++) {
                    Serial.printf("%02X ", (uint8_t)value[i]);
                }
                Serial.println();
                
                pAudioCharacteristic->setValue((uint8_t*)value.data(), len);
                pAudioCharacteristic->notify();
            }
        }
    }
};

// Audio streaming constants for ESP-NOW - FIT ESP-NOW LIMITS
#define AUDIO_CHUNK_SIZE 200  // 100 samples * 2 bytes = 200 bytes (fits in 250B ESP-NOW limit)
#define AUDIO_SAMPLE_RATE 16000  // Match Android: 16kHz
#define AUDIO_BITS_PER_SAMPLE 16  // Match Android: 16-bit
#define AUDIO_CHANNELS 1  // Match Android: Mono
#define AUDIO_COMPRESSION_RATIO 1  // No compression - raw PCM

// Audio streaming variables for ESP-NOW - RAW PCM
int audioBufferIndex = 0;
unsigned long lastAudioChunk = 0;
int audioSequenceNumber = 0;

// Queue to defer BLE onWrite processing out of BLE stack task
struct IncomingBleItem {
  uint16_t length;
  uint8_t data[256];
};
static volatile uint16_t bleInHead = 0;
static volatile uint16_t bleInTail = 0;
static IncomingBleItem bleInQueue[16];

static inline bool bleInPushFromISR(const uint8_t* buf, uint16_t len) {
  if (len > 256) len = 256;
  uint16_t nextHead = (uint16_t)((bleInHead + 1) & 0x0F);
  if (nextHead == bleInTail) return false; // full
  IncomingBleItem &slot = bleInQueue[bleInHead & 0x0F];
  slot.length = len;
  memcpy(slot.data, buf, len);
  __atomic_store_n(&bleInHead, nextHead, __ATOMIC_RELEASE);
  return true;
}

static inline bool bleInPop(IncomingBleItem &out) {
  uint16_t tail = __atomic_load_n(&bleInTail, __ATOMIC_ACQUIRE);
  uint16_t head = __atomic_load_n(&bleInHead, __ATOMIC_ACQUIRE);
  if (tail == head) return false; // empty
  IncomingBleItem &slot = bleInQueue[tail & 0x0F];
  out.length = slot.length;
  memcpy(out.data, slot.data, slot.length);
  __atomic_store_n(&bleInTail, (uint16_t)((tail + 1) & 0x0F), __ATOMIC_RELEASE);
  return true;
}

// Audio streaming functions
void startAudioStream() {
  if (!isAudioStreaming) {
    isAudioStreaming = true;
    audioSequenceNumber = 0;
    audioBufferIndex = 0;
    Serial.println("🎤 Audio streaming started");
    setStatusLED(0, 255, 0); // Green for active streaming
  }
}

void stopAudioStream() {
  if (isAudioStreaming) {
    isAudioStreaming = false;
    
    // Clear audio buffer when stopping
    memset(audioBuffer, 0, AUDIO_BUFFER_SIZE);
    audioBufferIndex = 0;
    audioSequenceNumber = 0;
    
    Serial.println("🛑 Audio streaming stopped - buffer cleared");
    updateMeshStatusLED(); // Use mesh status instead of hardcoded blue
  }
}

void addAudioData(const uint8_t* data, int length) {
  if (!isAudioStreaming) return;
  
  // Safety check: prevent buffer overflow
  if (length <= 0 || data == nullptr) {
    Serial.println("❌ Invalid audio data parameters");
    return;
  }
  
  // Check if we have enough space for new data
  if (audioBufferIndex + length > AUDIO_BUFFER_SIZE) {
    Serial.printf("⚠️ Buffer overflow prevented! Current: %d, Adding: %d, Max: %d\n", 
                  audioBufferIndex, length, AUDIO_BUFFER_SIZE);
    
    // Force send chunks to make space
    if (audioBufferIndex >= AUDIO_CHUNK_SIZE) {
      sendAudioChunks();
    }
    
    // If still no space, drop oldest data (FIFO behavior)
    if (audioBufferIndex + length > AUDIO_BUFFER_SIZE) {
      int overflow = (audioBufferIndex + length) - AUDIO_BUFFER_SIZE;
      Serial.printf("🔄 Dropping %d oldest bytes to prevent overflow\n", overflow);
      
      // Shift buffer left by overflow amount
      memmove(audioBuffer, audioBuffer + overflow, audioBufferIndex - overflow);
      audioBufferIndex -= overflow;
    }
  }
  
  // Add data to buffer safely
  memcpy(audioBuffer + audioBufferIndex, data, length);
  audioBufferIndex += length;
  
  Serial.printf("📥 Audio data added: %d bytes, buffer now: %d/%d bytes\n", 
                length, audioBufferIndex, AUDIO_BUFFER_SIZE);
  
  // If buffer has enough data for chunks, send them
  if (audioBufferIndex >= AUDIO_CHUNK_SIZE) {
    sendAudioChunks();
  }
}

// RAW PCM: No compression - direct data passthrough
int compressAudioData(const uint8_t* input, int inputLength, uint8_t* output) {
  if (inputLength <= 0 || input == nullptr || output == nullptr) return 0;
  
  // Direct copy - no compression
  memcpy(output, input, inputLength);
  return inputLength;
}

// RAW PCM: Simplified audio statistics (minimal overhead)
void calculateAudioStats(const uint8_t* data, int length, uint16_t& minVal, uint16_t& maxVal, uint32_t& avgVal) {
  if (length <= 0 || data == nullptr) return;
  
  // Minimal stats for compatibility - set to neutral values
  minVal = 128;
  maxVal = 128;
  avgVal = 128;
}

// u-law encoding function for an audio sample
uint8_t linearToUlaw(int16_t pcm_val) {
    int16_t mask;
    int16_t seg;
    uint8_t uval;
    pcm_val = pcm_val >> 2;
    if (pcm_val < 0) {
        pcm_val = -pcm_val;
        mask = 0x7F;
    } else {
        mask = 0xFF;
    }
    if (pcm_val > 8158) pcm_val = 8158;
    pcm_val += 133;

    seg = 0;
    while( pcm_val > ( (33<<(seg+1)) - 33 ) ){
        seg++;
    }

    uval = (uint8_t)(((seg << 4) | ((pcm_val - ( (33<<(seg)) - 33 )) >> (seg + 1))));
    return (uval ^ mask);
}


void sendBeepOnce() {
  Serial.println("--- Starting 5-second beep test (u-law compressed) ---");
  const int loopCount = 400; // 400 loops * 12.5ms/loop = 5000ms = 5s
  const int samplesPerLoop = 200;
  static int16_t sineFrame[samplesPerLoop];
  static bool sineInit = false;
  if (!sineInit) {
    for (int i = 0; i < samplesPerLoop; i++) {
        // 1kHz sine wave at 16kHz sample rate
        sineFrame[i] = (int16_t)(sin(2 * PI * 1000.0 * i / AUDIO_SAMPLE_RATE) * 16384.0);
    }
    sineInit = true;
  }

  const TickType_t xFrequency = pdMS_TO_TICKS(12); // Paced for ~12.5ms
  TickType_t xLastWakeTime = xTaskGetTickCount();

  for (int f = 0; f < loopCount; f++) {
    uint8_t ulawBuffer[samplesPerLoop];
    for (int i = 0; i < samplesPerLoop; i++) {
        ulawBuffer[i] = linearToUlaw(sineFrame[i]);
    }

    if (deviceConnected && pAudioCharacteristic != nullptr) {
      pAudioCharacteristic->setValue(ulawBuffer, samplesPerLoop);
      pAudioCharacteristic->notify();
    }
    
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
  }
  Serial.println("--- Beep test finished ---");
}

void sendAudioChunks() {
  if (meshDeviceCount == 0 && !deviceConnected) {
    Serial.println("⚠️ No mesh or BLE devices connected, clearing audio buffer");
    memset(audioBuffer, 0, AUDIO_BUFFER_SIZE);
    audioBufferIndex = 0;
    return;
  }
  
  // Buffer health check
  // Reduced logging to avoid heap churn during high-rate streams
  // Serial.printf("🔍 Buffer health: %d/%d bytes (%.1f%% full)\n", audioBufferIndex, AUDIO_BUFFER_SIZE, (float)audioBufferIndex / AUDIO_BUFFER_SIZE * 100.0);
  
  int chunksToSend = audioBufferIndex / AUDIO_CHUNK_SIZE;
  
  for (int chunk = 0; chunk < chunksToSend; chunk++) {
        // RAW PCM: Direct audio data transmission
        int startIndex = chunk * AUDIO_CHUNK_SIZE;
        uint8_t rawBuffer[AUDIO_CHUNK_SIZE];
        int rawSize = compressAudioData(audioBuffer + startIndex, AUDIO_CHUNK_SIZE, rawBuffer);
        
        // Calculate audio statistics efficiently
        uint16_t minVal, maxVal;
        uint32_t avgVal;
        calculateAudioStats(audioBuffer + startIndex, AUDIO_CHUNK_SIZE, minVal, maxVal, avgVal);
        
        // Create raw PCM audio chunk message
        char messageBuffer[300];  // Pre-allocated buffer for efficiency
        int messageLen = snprintf(messageBuffer, sizeof(messageBuffer),
                                 "P:%d:%d:%d:%d:%d:%d:%d:%d",
                                 audioSequenceNumber++, chunk, chunksToSend,
                                 millis(), AUDIO_SAMPLE_RATE, AUDIO_BITS_PER_SAMPLE,
                                 minVal, maxVal);
        
        // Add raw PCM audio data directly
        if (messageLen + rawSize + 1 < sizeof(messageBuffer)) {
            messageBuffer[messageLen++] = ':';
            memcpy(messageBuffer + messageLen, rawBuffer, rawSize);
            messageLen += rawSize;
        }
        
        // CRITICAL: Ensure message fits within ESP-NOW limits (250 bytes)
        // Note: With 200-byte audio chunks, they fit within ESP-NOW limits
        if (messageLen > 250) {
          Serial.printf("⚠️ Message too large for ESP-NOW: %d bytes (max 250)\n", messageLen);
          // Split large audio chunks into smaller pieces
          int maxAudioData = 250 - messageLen + rawSize; // Available space for audio data
          if (maxAudioData > 0) {
            messageLen = messageLen - rawSize + maxAudioData; // Adjust message length
            memcpy(messageBuffer + (messageLen - maxAudioData), rawBuffer, maxAudioData);
          } else {
            Serial.printf("❌ Header too large, skipping chunk\n");
            continue;
          }
        }
        
        // Send to all mesh devices
        if (meshDeviceCount > 0) {
          for (int i = 0; i < meshDeviceCount; i++) {
            if (meshDevices[i].isActive) {
              esp_err_t result = esp_now_send(meshDevices[i].mac, 
                                             (uint8_t*)messageBuffer, 
                                             messageLen);
              (void)result;
            }
          }
        }
        
        // CRITICAL FIX: Also send audio data to BLE characteristic for Android app
        if (deviceConnected && pAudioCharacteristic != nullptr) {
          // Send raw audio data directly to BLE characteristic
          pAudioCharacteristic->setValue(rawBuffer, rawSize);
          pAudioCharacteristic->notify();
          Serial.printf("📱 Audio sent to BLE characteristic: %d bytes\n", rawSize);
        }
    
    lastAudioChunk = millis();
    // NOTE: A hardcoded delay was removed from here. The main loop's delay provides general pacing.
    // For high-throughput streaming, a more sophisticated pacing mechanism would be needed here,
    // similar to the micros() loop in sendBeepOnce().
  }
  
  // Clear sent data from buffer - OPTIMIZED VERSION
  int remainingData = audioBufferIndex % AUDIO_CHUNK_SIZE;
  if (remainingData > 0) {
    // Use memmove for efficient buffer shifting (safer than manual loop)
    memmove(audioBuffer, audioBuffer + (chunksToSend * AUDIO_CHUNK_SIZE), remainingData);
    audioBufferIndex = remainingData;
    
    // Clear the unused portion of buffer to prevent data leakage
    memset(audioBuffer + remainingData, 0, AUDIO_BUFFER_SIZE - remainingData);
  } else {
    // Clear entire buffer when no remaining data
    memset(audioBuffer, 0, AUDIO_BUFFER_SIZE);
    audioBufferIndex = 0;
  }
  
  Serial.printf("🧹 Buffer cleared: %d bytes remaining, index reset to %d\n", remainingData, audioBufferIndex);
}

// Enhanced test command handler for audio testing and data validation
void handleTestCommand(const String& command) {
  if (command == "buffer_status") {
    // Show buffer status
    Serial.printf("📊 OPTIMIZED BUFFER STATUS:\n");
    Serial.printf("   Buffer Size: %d bytes\n", AUDIO_BUFFER_SIZE);
    Serial.printf("   Used: %d bytes\n", audioBufferIndex);
    Serial.printf("   Free: %d bytes\n", AUDIO_BUFFER_SIZE - audioBufferIndex);
    Serial.printf("   Chunk Size: %d bytes\n", AUDIO_CHUNK_SIZE);
    Serial.printf("   Max Chunks: %d\n", AUDIO_BUFFER_SIZE / AUDIO_CHUNK_SIZE);
    Serial.printf("   Current Chunks: %d\n", audioBufferIndex / AUDIO_CHUNK_SIZE);
    Serial.printf("   Streaming: %s\n", isAudioStreaming ? "Yes" : "No");
    
    // Show audio quality parameters
    Serial.printf("   Audio Quality:\n");
    Serial.printf("     Sample Rate: %d Hz\n", AUDIO_SAMPLE_RATE);
    Serial.printf("     Bits per Sample: %d\n", AUDIO_BITS_PER_SAMPLE);
    Serial.printf("     Channels: %d\n", AUDIO_CHANNELS);
    Serial.printf("     Raw Data Rate: %d KB/s\n", (AUDIO_SAMPLE_RATE * AUDIO_BITS_PER_SAMPLE * AUDIO_CHANNELS) / 8000);
    Serial.printf("     Compression Ratio: 1:%d\n", AUDIO_COMPRESSION_RATIO);
    
    // Show mesh status
    Serial.printf("   Mesh Network:\n");
    Serial.printf("     Active: %s\n", meshNetworkActive ? "Yes" : "No");
    Serial.printf("     Devices: %d/%d\n", meshDeviceCount, MAX_MESH_DEVICES);
    
    // Show first few bytes of buffer content
    Serial.printf("   Buffer Preview: ");
    for (int i = 0; i < min(16, (int)audioBufferIndex); i++) {
      Serial.printf("%02X ", audioBuffer[i]);
    }
    Serial.println();
    
  } else if (command == "test_compression") {
    // Test compression with sample data
    Serial.println("🧪 Testing audio compression...");
    
    // Generate test audio data
    uint8_t testData[240];
    for (int i = 0; i < 240; i++) {
      testData[i] = random(0, 255);
    }
    
    // Test compression
    uint8_t testBuffer[240];
    int compressedSize = compressAudioData(testData, 240, testBuffer);
    float compressionRatio = (float)240 / compressedSize;
    float compressionPercent = (float)(240 - compressedSize) / 240 * 100.0;
    
    Serial.printf("🗜️ Compression Test Results:\n");
    Serial.printf("   Original: 240 bytes\n");
    Serial.printf("   Compressed: %d bytes\n", compressedSize);
    Serial.printf("   Compression Ratio: 1:%.2f\n", compressionRatio);
    Serial.printf("   Space Saved: %.1f%%\n", compressionPercent);
    
  } else if (command == "start_audio_stream") {
    Serial.println("🎵 Starting audio stream...");
    startAudioStream();
    
  } else if (command == "stop_audio_stream") {
    Serial.println("⏹️ Stopping audio stream...");
    stopAudioStream();
    
  } else if (command.startsWith("send_audio_chunk:")) {
    // Parse: send_audio_chunk:chunk_id:hex_data
    int firstColon = command.indexOf(':');
    int secondColon = command.indexOf(':', firstColon + 1);
    
    if (firstColon != -1 && secondColon != -1) {
      String chunkId = command.substring(firstColon + 1, secondColon);
      String hexData = command.substring(secondColon + 1);
      
      Serial.printf("📤 Sending test audio chunk: %s (hex data: %s)\n", chunkId.c_str(), hexData.c_str());
      
      // Convert hex to bytes
      int dataLength = hexData.length() / 2;
      if (dataLength > 0 && dataLength <= AUDIO_CHUNK_SIZE) {
        uint8_t testData[AUDIO_CHUNK_SIZE];
        int actualLength = 0;
        
        // Parse hex string
        for (int i = 0; i < dataLength && i < AUDIO_CHUNK_SIZE; i++) {
          if (i * 2 + 1 < hexData.length()) {
            String byteStr = hexData.substring(i * 2, i * 2 + 2);
            testData[i] = strtol(byteStr.c_str(), NULL, 16);
            actualLength++;
          }
        }
        
        Serial.printf("Parsed %d bytes from hex data\n", actualLength);
        
        // Store in buffer and send
        if (audioBufferIndex + actualLength <= AUDIO_BUFFER_SIZE) {
          memcpy(audioBuffer + audioBufferIndex, testData, actualLength);
          audioBufferIndex += actualLength;
          Serial.printf("Test audio chunk '%s' added to buffer (total: %d bytes)\n", chunkId.c_str(), audioBufferIndex);
          
          // Send immediately for validation
          sendAudioChunks();
        } else {
          Serial.println("Buffer full, cannot add test chunk");
        }
      } else {
        Serial.printf("Invalid data length: %d bytes (max: %d)\n", dataLength, AUDIO_CHUNK_SIZE);
      }
    } else {
      Serial.println("Invalid format. Use: send_audio_chunk:chunk_id:hex_data");
    }
    
  } else if (command == "clear_buffer") {
    // Clear audio buffer
    memset(audioBuffer, 0, AUDIO_BUFFER_SIZE);
    audioBufferIndex = 0;
    audioSequenceNumber = 0;
    Serial.println("🧹 Audio buffer cleared");
  } else if (command == "send_beep") {
    sendBeepOnce();
  } else if (command.startsWith("send_ping:")) {
    String text = command.substring(strlen("send_ping:"));
    if (text.length() == 0) {
      Serial.println("Usage: send_ping:<text>");
      return;
    }
    static uint32_t pingSeq = 0;
    char header[32];
    int headerLen = snprintf(header, sizeof(header), "P:%lu:", (unsigned long)pingSeq++);
    // Build message: header + text (binary append)
    char msg[128];
    int msgLen = 0;
    if (headerLen + text.length() < (int)sizeof(msg)) {
      memcpy(msg, header, headerLen);
      msgLen = headerLen;
      memcpy(msg + msgLen, text.c_str(), text.length());
      msgLen += text.length();
      Serial.printf("📡 Sending PING '%s' (%d bytes) to %d mesh devices\n", text.c_str(), msgLen, meshDeviceCount);
      for (int i = 0; i < meshDeviceCount; i++) {
        if (meshDevices[i].isActive) {
          esp_err_t res = esp_now_send(meshDevices[i].mac, (uint8_t*)msg, msgLen);
          (void)res;
        }
      }
    } else {
      Serial.println("PING too large");
    }
  } else {
    Serial.printf("Unknown command: %s\n", command.c_str());
    Serial.println("Available commands: buffer_status, test_compression, start_audio_stream, stop_audio_stream, send_audio_chunk:chunk_id:hex_data, clear_buffer");
  }
}

void setup() {
  // Initialize serial communication
  Serial.begin(115200);
  Serial.println("\n\n=== ESP32-S3 BLE AUDIO SERVER STARTING ===");
  
  // Initialize Neopixel LED
  pixels.begin();
  pixels.setBrightness(50); // Set brightness to 50%
  setStatusLED(0, 0, 255); // Blue during startup
  
  // Initialize GPIO pins
  pinMode(CONNECTION_LED_PIN, OUTPUT);
  digitalWrite(CONNECTION_LED_PIN, LOW);
  
  // Initialize ESP-NOW Mesh
  setupESPNOWMesh();
  
  // Initialize BLE
  Serial.println("Initializing BLE...");
  // Free Classic BT memory for BLE-only stability
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setPower(ESP_PWR_LVL_P9);
  
  // Create the BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  
  // Create the BLE Service
  BLEService *pService = pServer->createService(SERVICE_UUID);
  
  // Create BLE Characteristic
  pAudioCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ   |
                      BLECharacteristic::PROPERTY_WRITE  |
                      BLECharacteristic::PROPERTY_NOTIFY |
                      BLECharacteristic::PROPERTY_INDICATE
                    );
  
  // Add descriptor for notifications
  pAudioCharacteristic->addDescriptor(new BLE2902());
  
  // Set callbacks
  pAudioCharacteristic->setCallbacks(new MyCharacteristicCallbacks());
  
  // Start the service
  pService->start();
  
  // Start advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  
  // Build the advertisement data packet
  BLEAdvertisementData advData;
  advData.setFlags(0x06); // GENERAL_DISC_MODE | BR_EDR_NOT_SUPPORTED
  advData.setServiceData(BLEUUID(SERVICE_UUID), "audio");
  advData.setManufacturerData(std::string("WM"));
  pAdvertising->setAdvertisementData(advData);

  // Build the scan response packet
  BLEAdvertisementData scanData;
  scanData.setName(DEVICE_NAME);
  pAdvertising->setScanResponseData(scanData);
  
  pAdvertising->setMinPreferred(0x0);  // keep minimal preferred params
  BLEDevice::startAdvertising();
  
  Serial.println("=== BLE SERVER READY ===");
  Serial.printf("Device name: %s\n", DEVICE_NAME);
  Serial.printf("Service UUID: %s\n", SERVICE_UUID);
  Serial.printf("Characteristic UUID: %s\n", CHARACTERISTIC_UUID);
  Serial.println("Waiting for connections...");
  
  // Blink status LED to indicate ready
  blinkStatusLED(0, 255, 255, 3); // Cyan blink when ready

  // Create the dedicated audio sender task
  xTaskCreatePinnedToCore(
      AudioSenderTask,          /* Task function. */
      "AudioSender",            /* name of task. */
      4096,                     /* Stack size of task */
      NULL,                     /* parameter of the task */
      1,                        /* priority of the task */
      &AudioSenderTaskHandle,   /* Task handle to keep track of created task */
      1);                       /* pin task to core 1 */

}

void loop() {
  // Handle BLE connection state changes
  if (pendingBeep) { pendingBeep = false; sendBeepOnce(); }
  // Drain BLE incoming queue
  {
    IncomingBleItem item;
    int processed = 0;
    while (processed < 8 && bleInPop(item)) {
      addAudioData(item.data, item.length);
      processed++;
    }
  }
  if (!deviceConnected && oldDeviceConnected) {
    delay(500); // give the bluetooth stack the chance to get things ready
    pServer->startAdvertising(); // restart advertising
    Serial.println("Restart advertising");
    oldDeviceConnected = deviceConnected;
  }
  
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }
  
  // Handle BLE connections
  if (deviceConnected) {
    // Audio processing now handled by addAudioData() → sendAudioChunks() flow
    // No need to process audioBuffer here anymore
  }
  
  // Handle test commands from serial monitor
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    
    if (command.length() > 0) {
      Serial.printf("📥 Received command: %s\n", command.c_str());
      handleTestCommand(command);
    }
  }
  
  // Audio streaming management is now handled by the dedicated AudioSenderTask
  // if (isAudioStreaming) {
  //   sendAudioChunks();
  // }
  
  // Mesh network management
  if (meshNetworkActive) {
    // Send heartbeat to all mesh devices
    static unsigned long lastHeartbeat = 0;
    if (millis() - lastHeartbeat >= MESH_HEARTBEAT_INTERVAL) {
      sendMeshHeartbeat();
      lastHeartbeat = millis();
    }
    
    // Broadcast mesh status
    static unsigned long lastStatusBroadcast = 0;
    if (millis() - lastStatusBroadcast >= MESH_BROADCAST_INTERVAL) {
      broadcastMeshStatus();
      lastStatusBroadcast = millis();
    }
    
    // Cleanup inactive devices and update LED
    static unsigned long lastCleanup = 0;
    if (millis() - lastCleanup >= DEVICE_TIMEOUT) {
      cleanupInactiveDevices();
      updateMeshStatusLED();
      lastCleanup = millis();
    }
  }
  
  // Print statistics periodically
  static unsigned long lastStats = 0;
  if (millis() - lastStats >= 10000) {  // Every 10 seconds
    printStatistics();
    lastStats = millis();
  }
  
  // Small delay to prevent watchdog issues
  delay(10);
}
