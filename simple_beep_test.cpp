// SIMPLE BEEP TEST - MINIMAL IMPLEMENTATION
// This is what the audio pipeline SHOULD look like

// ESP32 A (Coordinator) - SIMPLE VERSION
void sendSimpleBeep() {
  if (meshDeviceCount == 0) return;
  
  // Generate 1 second of 1kHz tone (16000 samples at 16kHz)
  const int samples = 16000;  // 1 second
  const int frequency = 1000; // 1kHz
  
  for (int i = 0; i < samples; i++) {
    // Generate 16-bit PCM sample
    float t = (float)i / 16000.0f;
    float sample = sinf(2.0f * 3.14159f * frequency * t);
    int16_t pcmSample = (int16_t)(sample * 16383.0f);
    
    // Convert to bytes (little-endian)
    uint8_t audioBytes[2];
    audioBytes[0] = pcmSample & 0xFF;        // Low byte
    audioBytes[1] = (pcmSample >> 8) & 0xFF; // High byte
    
    // Send to ESP32 B via ESP-NOW
    esp_now_send(meshDevices[0].mac, audioBytes, 2);
    
    // Wait for next sample (62.5 microseconds)
    delayMicroseconds(62);
  }
}

// ESP32 B (Client) - SIMPLE VERSION  
void OnDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len == 2) { // Expecting 2 bytes per sample
    // Forward directly to Phone B via BLE
    pAudioCharacteristic->setValue(data, 2);
    pAudioCharacteristic->notify();
  }
}

// Android - SIMPLE VERSION
fun playBeep() {
  // Just play the received 16-bit PCM data directly
  audioTrack?.write(receivedData, 0, receivedData.size)
}
