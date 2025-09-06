package com.example.bleaudio

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.Build
import android.util.Log
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

class BLEAudioManager(
    private val context: Context,
    private val onConnectionStateChanged: (Boolean) -> Unit,
    private val onDeviceFound: (BluetoothDevice) -> Unit,
    private val onError: (String) -> Unit,
    private val onAudioDataReceived: (ByteArray, Int) -> Unit
) {
    companion object {
        private const val TAG = "BLEAudioManager"
        private const val SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
        private const val CHARACTERISTIC_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
        private const val DESCRIPTOR_UUID = "00002902-0000-1000-8000-00805f9b34fb"
        private const val DEVICE_NAME_PREFIX = "ESP32S3"
        private const val SCAN_TIMEOUT_MS = 10000L
        private const val CONNECTION_TIMEOUT_MS = 10000L
        private const val MAX_RETRY_ATTEMPTS = 3
        private const val RETRY_DELAY_MS = 100L
        private const val TARGET_MTU = 512
    }

    private val bluetoothManager: BluetoothManager? = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
    private val bluetoothAdapter: BluetoothAdapter? = bluetoothManager?.adapter
    private val bluetoothLeScanner: BluetoothLeScanner? = bluetoothAdapter?.bluetoothLeScanner
    
    private var bluetoothGatt: BluetoothGatt? = null
    private var audioCharacteristic: BluetoothGattCharacteristic? = null
    private var selectedDevice: BluetoothDevice? = null
    private var negotiatedMtu: Int = 23
    private var mtuReady: Boolean = false
    private val maxFrameSizeBytes: Int = 200
    private val rxFrameBuffer = ByteArray(maxFrameSizeBytes)
    private var rxFrameIndex: Int = 0
    private var currentFrameSizeBytes: Int = 200
    private var startupFrameUntilMs: Long = 0L
    
    private val isScanning = AtomicBoolean(false)
    private val isConnected = AtomicBoolean(false)
    private val retryAttempts = AtomicInteger(0)
    
    private val mainHandler = Handler(Looper.getMainLooper())
    private var connectionTimeoutRunnable: Runnable? = null
    private var scanTimeoutRunnable: Runnable? = null
    
    private val foundDevices = mutableListOf<BluetoothDevice>()
    
    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device
            val deviceName = device.name
            
            Log.d(TAG, "Scan result: $deviceName (${device.address})")
            
            val hasService = result.scanRecord?.serviceUuids?.any { it.uuid.toString().equals(SERVICE_UUID, true) } == true
            if (hasService || (deviceName != null && deviceName.startsWith(DEVICE_NAME_PREFIX))) {
                Log.d(TAG, "Found ESP32 device: $deviceName")
                
                // Check if device already exists
                if (!foundDevices.any { it.address == device.address }) {
                    foundDevices.add(device)
                    onDeviceFound(device)
                }
            }
        }
        
        override fun onScanFailed(errorCode: Int) {
            Log.e(TAG, "Scan failed with error: $errorCode")
            isScanning.set(false)
            onError("Scan failed with error: $errorCode")
        }
    }
    
    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            Log.d(TAG, "Connection state changed: status=$status, newState=$newState")
            
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    Log.d(TAG, "=== SUCCESSFULLY CONNECTED TO GATT SERVER ===")
                    Log.d(TAG, "Device: ${gatt.device.name} (${gatt.device.address})")
                    Log.d(TAG, "Connection status: $status")
                    
                    // Clear connection timeout
                    connectionTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
                    connectionTimeoutRunnable = null
                    
                    isConnected.set(true)
                    onConnectionStateChanged(true)
                    
                    // Improve initial link parameters for faster service discovery and MTU
                    try {
                        gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
                    } catch (e: Exception) {
                        Log.w(TAG, "Failed to set connection priority: ${e.message}")
                    }

                    // Request MTU for better performance. Service discovery will be triggered in onMtuChanged.
                    Log.d(TAG, "Requesting MTU: $TARGET_MTU")
                    mtuReady = false
                    negotiatedMtu = 23
                    mainHandler.postDelayed({
                        gatt.requestMtu(TARGET_MTU)
                    }, 200) // Small delay for stability
                }
                
                BluetoothProfile.STATE_DISCONNECTED -> {
                    Log.d(TAG, "=== DISCONNECTED FROM GATT SERVER ===")
                    Log.d(TAG, "Disconnection status: $status")
                    
                    isConnected.set(false)
                    audioCharacteristic = null
                    retryAttempts.set(0)
                    onConnectionStateChanged(false)
                    
                    // Try to reconnect if this was an unexpected disconnection
                    if (status != 133) { // 133 = GATT_ERROR, normal disconnection
                        Log.d(TAG, "Attempting to reconnect...")
                        mainHandler.postDelayed({
                            selectedDevice?.let { connectToDevice(it) }
                        }, 1000)
                    }
                }
            }
        }
        
        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            Log.d(TAG, "MTU changed: $mtu (status: $status)")
            if (status == BluetoothGatt.GATT_SUCCESS) {
                Log.d(TAG, "=== MTU NEGOTIATION SUCCESSFUL ===")
                Log.d(TAG, "New MTU size: $mtu")
                negotiatedMtu = mtu
                mtuReady = true
                // Trigger service discovery after MTU negotiation is complete
                Log.d(TAG, "Discovering services...")
                mainHandler.postDelayed({
                    gatt.discoverServices()
                }, 200) // Small delay for stability
            } else {
                Log.w(TAG, "MTU negotiation failed: $status")
            }
        }
        
        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            Log.d(TAG, "Services discovered: status=$status")
            
            if (status == BluetoothGatt.GATT_SUCCESS) {
                Log.d(TAG, "=== SERVICES DISCOVERED SUCCESSFULLY ===")
                
                val service = gatt.getService(java.util.UUID.fromString(SERVICE_UUID))
                if (service != null) {
                    Log.d(TAG, "Found audio service: ${service.uuid}")
                    
                    audioCharacteristic = service.getCharacteristic(java.util.UUID.fromString(CHARACTERISTIC_UUID))
                    if (audioCharacteristic != null) {
                        Log.d(TAG, "=== CHARACTERISTIC FOUND ===")
                        Log.d(TAG, "Characteristic UUID: ${audioCharacteristic!!.uuid}")
                        Log.d(TAG, "Properties: ${audioCharacteristic!!.properties}")
                        
                        // Enable notifications
                        val descriptor = audioCharacteristic!!.getDescriptor(java.util.UUID.fromString(DESCRIPTOR_UUID))
                        if (descriptor != null) {
                            Log.d(TAG, "Enabling notifications...")
                            // First enable characteristic notifications
                            gatt.setCharacteristicNotification(audioCharacteristic!!, true)
                            // Then write the descriptor
                            descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                            gatt.writeDescriptor(descriptor)
                        } else {
                            Log.w(TAG, "Descriptor not found")
                        }
                    } else {
                        Log.e(TAG, "Audio characteristic not found!")
                        onError("Audio characteristic not found")
                    }
                } else {
                    Log.e(TAG, "Audio service not found!")
                    onError("Audio service not found")
                }
            } else {
                Log.e(TAG, "Service discovery failed: $status")
                onError("Service discovery failed: $status")
            }
        }
        
        override fun onDescriptorWrite(gatt: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            Log.d(TAG, "Descriptor write: status=$status")
            if (status == BluetoothGatt.GATT_SUCCESS) {
                Log.d(TAG, "=== NOTIFICATIONS ENABLED SUCCESSFULLY ===")
                Log.d(TAG, "Ready to receive audio data")
                // Ensure clean reassembly state at the moment notifications start
                rxFrameIndex = 0
                java.util.Arrays.fill(rxFrameBuffer, 0.toByte())
                // Use smaller startup frames for ~500ms to avoid initial join
                currentFrameSizeBytes = 100
                startupFrameUntilMs = System.currentTimeMillis() + 500
            } else {
                Log.w(TAG, "Failed to enable notifications: $status")
            }
        }
        
        override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            val data = characteristic.value
            val size = data.size
            
            Log.d(TAG, "=== CHARACTERISTIC CHANGED ===")
            Log.d(TAG, "Received data: $size bytes")
            Log.d(TAG, "First 8 bytes: ${data.take(8).joinToString { "0x%02X".format(it) }}")
            
            if (size > 0) {
                ingestAndEmitFrames(data)
            }
        }
        
        override fun onCharacteristicWrite(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            Log.d(TAG, "Characteristic write: status=$status")
            if (status == BluetoothGatt.GATT_SUCCESS) {
                Log.d(TAG, "=== CHARACTERISTIC WRITE SUCCESS ===")
            } else {
                Log.e(TAG, "Characteristic write failed: $status")
            }
        }
    }
    
    fun isInitialized(): Boolean {
        return bluetoothManager != null && bluetoothAdapter != null
    }
    
    fun isScanning(): Boolean = isScanning.get()
    fun isConnected(): Boolean = isConnected.get()
    
    fun startScan() {
        if (!isInitialized()) {
            onError("Bluetooth not initialized")
            return
        }
        
        if (isScanning.get()) {
            Log.d(TAG, "Scan already in progress")
            return
        }
        
        Log.d(TAG, "Starting BLE scan...")
        foundDevices.clear()
        isScanning.set(true)
        
        // Set scan timeout
        scanTimeoutRunnable = Runnable {
            Log.d(TAG, "Scan timeout reached")
            stopScan()
        }
        mainHandler.postDelayed(scanTimeoutRunnable!!, SCAN_TIMEOUT_MS)
        
        try {
            if (bluetoothLeScanner != null) {
                // Use modern BLE scanner
                val scanSettings = ScanSettings.Builder()
                    .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                    .build()
                
                bluetoothLeScanner.startScan(null, scanSettings, scanCallback)
                Log.d(TAG, "Modern BLE scan started")
            } else {
                // Fallback to legacy scanner
                Log.d(TAG, "Using legacy BLE scanner")
                bluetoothAdapter?.startLeScan(leScanCallback)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error starting scan", e)
            isScanning.set(false)
            onError("Scan error: ${e.message}")
        }
    }
    
    fun stopScan() {
        Log.d(TAG, "Stopping BLE scan...")
        
        scanTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
        scanTimeoutRunnable = null
        
        try {
            if (bluetoothLeScanner != null) {
                bluetoothLeScanner.stopScan(scanCallback)
                Log.d(TAG, "Modern BLE scan stopped")
            } else {
                bluetoothAdapter?.stopLeScan(leScanCallback)
                Log.d(TAG, "Legacy BLE scan stopped")
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error stopping scan", e)
        }
        
        isScanning.set(false)
    }
    
    fun connectToDevice(device: BluetoothDevice) {
        Log.d(TAG, "Connecting to device: ${device.name} (${device.address})")
        
        selectedDevice = device
        
        // Stop scanning before attempting to connect for better reliability
        if (isScanning.get()) {
            stopScan()
        }

        // Set connection timeout
        connectionTimeoutRunnable = Runnable {
            Log.w(TAG, "=== CONNECTION TIMEOUT ===")
            onError("Connection timeout after ${CONNECTION_TIMEOUT_MS}ms")
        }
        mainHandler.postDelayed(connectionTimeoutRunnable!!, CONNECTION_TIMEOUT_MS)
        
        // Connect to device with autoConnect=false for faster connection
        Log.d(TAG, "Calling connectGatt...")
        bluetoothGatt = try {
            if (Build.VERSION.SDK_INT >= 23) {
                device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
            } else {
                device.connectGatt(context, false, gattCallback)
            }
        } catch (e: Exception) {
            Log.e(TAG, "connectGatt failed: ${e.message}")
            null
        }
        Log.d(TAG, "connectGatt invoked (autoConnect=false, LE transport if available)")
    }
    
    fun disconnect() {
        Log.d(TAG, "Disconnecting from device")
        
        // Clear timeouts
        connectionTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
        connectionTimeoutRunnable = null
        
        // Stop scanning if active
        stopScan()
        
        // Disconnect GATT
        try {
            bluetoothGatt?.disconnect()
            bluetoothGatt?.close()
            bluetoothGatt = null
        } catch (e: Exception) {
            Log.e(TAG, "Error disconnecting", e)
        }
        
        isConnected.set(false)
        audioCharacteristic = null
        retryAttempts.set(0)
    }
    
    fun sendAudioData(data: ByteArray, size: Int): Boolean {
        Log.d(TAG, "=== BLEAudioManager.sendAudioData CALLED ===")
        Log.d(TAG, "isConnected: ${isConnected.get()}")
        Log.d(TAG, "audioCharacteristic null: ${audioCharacteristic == null}")
        Log.d(TAG, "bluetoothGatt null: ${bluetoothGatt == null}")
        
        if (!isConnected.get()) {
            Log.w(TAG, "Not connected - attempting to reconnect")
            return false
        }
        
        if (audioCharacteristic == null) {
            Log.w(TAG, "Characteristic not available - checking connection")
            bluetoothGatt?.discoverServices()
            return false
        }
        try {
            Log.d(TAG, "=== SENDING AUDIO DATA (MTU-AWARE CHUNKING) ===")
            Log.d(TAG, "Data size: $size bytes, negotiatedMtu=$negotiatedMtu, mtuReady=$mtuReady")
            val maxPayload = kotlin.math.max(20, negotiatedMtu - 3)
            audioCharacteristic!!.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            var offset = 0
            while (offset < size) {
                val end = kotlin.math.min(offset + kotlin.math.min(currentFrameSizeBytes, maxPayload), size)
                val slice = data.copyOfRange(offset, end)
                audioCharacteristic!!.setValue(slice)
                val ok = bluetoothGatt?.writeCharacteristic(audioCharacteristic!!) ?: false
                if (!ok) {
                    Log.e(TAG, "Chunk write failed at offset=$offset size=${end - offset}")
                    return false
                }
                offset = end
                // Small pacing to avoid congesting the ATT queue
                Thread.sleep(5)
            }
            return true
        } catch (e: Exception) {
            Log.e(TAG, "=== AUDIO DATA SEND EXCEPTION ===")
            Log.e(TAG, "Exception during send: ${e.message}")
            Log.e(TAG, "Exception type: ${e.javaClass.simpleName}")
            e.printStackTrace()
            return false
        }
    }
    
    private fun processReceivedChunk(chunk: ByteArray) {
        // Ignore keep-alive packets (1 byte with value 0x00)
        if (chunk.size == 1 && chunk[0] == 0x00.toByte()) {
            Log.v(TAG, "Received keep-alive packet")
            return
        }
        
        // ESP32 is now sending complete audio frames (200 bytes)
        // No need for buffering - send directly to audio playback
        Log.d(TAG, "=== COMPLETE AUDIO FRAME RECEIVED ===")
        Log.d(TAG, "Received ${chunk.size} bytes (complete frame)")
        Log.d(TAG, "First 8 bytes: ${chunk.take(8).joinToString { "0x%02X".format(it) }}")
        
        // Send complete frame directly to audio playback
        onAudioDataReceived(chunk, chunk.size)
        
        Log.d(TAG, "=== COMPLETE FRAME SENT TO AUDIO PLAYBACK ===")
    }
    
    // Legacy scan callback for older Android devices
    private val leScanCallback = BluetoothAdapter.LeScanCallback { device, rssi, scanRecord ->
        val deviceName = device.name
        Log.d(TAG, "Legacy scan result: $deviceName (${device.address})")
        
        if (deviceName != null && deviceName.startsWith(DEVICE_NAME_PREFIX)) {
            Log.d(TAG, "Found ESP32 device (legacy): $deviceName")
            
            // Check if device already exists
            if (!foundDevices.any { it.address == device.address }) {
                foundDevices.add(device)
                onDeviceFound(device)
            }
        }
    }

    // Convenience method to send a test beep control packet
    fun sendTestBeep(): Boolean {
        val payload = "BEEP".toByteArray()
        val ok = sendAudioData(payload, payload.size)
        return ok
    }

    // Reassemble incoming notification chunks into 200-byte frames before playback
    private fun ingestAndEmitFrames(chunk: ByteArray) {
        // If startup window elapsed and we are at frame boundary, switch to 200B
        if (startupFrameUntilMs != 0L && System.currentTimeMillis() >= startupFrameUntilMs && rxFrameIndex == 0 && currentFrameSizeBytes != 200) {
            currentFrameSizeBytes = 200
        }
        var offset = 0
        while (offset < chunk.size) {
            val target = currentFrameSizeBytes
            val copyLen = kotlin.math.min(target - rxFrameIndex, chunk.size - offset)
            System.arraycopy(chunk, offset, rxFrameBuffer, rxFrameIndex, copyLen)
            rxFrameIndex += copyLen
            offset += copyLen
            if (rxFrameIndex == target) {
                val frame = rxFrameBuffer.copyOf(target)
                onAudioDataReceived(frame, frame.size)
                rxFrameIndex = 0
                // After emitting at boundary, re-check if we can switch to steady-state size
                if (startupFrameUntilMs != 0L && System.currentTimeMillis() >= startupFrameUntilMs && currentFrameSizeBytes != 200) {
                    currentFrameSizeBytes = 200
                }
            }
        }
    }
}
