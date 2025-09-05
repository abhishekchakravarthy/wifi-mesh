package com.example.bleaudio

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.util.Log
import android.widget.Button
import android.widget.TextView
import android.widget.Spinner
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AlertDialog
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import android.bluetooth.BluetoothDevice

class MainActivity : AppCompatActivity() {
    companion object {
        private const val TAG = "MainActivity"
        private const val REQUEST_ENABLE_BT = 1
        private const val REQUEST_PERMISSIONS = 2
    }

    private lateinit var bleAudioManager: BLEAudioManager
    private lateinit var audioCaptureManager: AudioCaptureManager
    private lateinit var statusText: TextView
    private lateinit var recordButton: Button
    private lateinit var scanButton: Button
    private lateinit var connectButton: Button
    private lateinit var testButton: Button
    private lateinit var deviceSpinner: Spinner
    private var isConnected = false
    private var isRecording = false
    private val foundDevices = mutableListOf<DeviceInfo>()
    private lateinit var deviceAdapter: ArrayAdapter<DeviceInfo>

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        // Initialize UI
        statusText = findViewById(R.id.statusText)
        recordButton = findViewById(R.id.recordButton)
        scanButton = findViewById(R.id.scanButton)
        connectButton = findViewById(R.id.connectButton)
        testButton = findViewById(R.id.testButton)
        deviceSpinner = findViewById(R.id.deviceSpinner)
        
        // Initialize device adapter
        deviceAdapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, foundDevices)
        deviceAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        deviceSpinner.adapter = deviceAdapter

        // Check permissions
        Log.d(TAG, "Checking permissions...")
        if (!checkPermissions()) {
            Log.w(TAG, "Some permissions are missing, requesting them...")
            requestPermissions()
        } else {
            Log.d(TAG, "All permissions are already granted")
        }

        // Initialize managers
        initializeManagers()

        // Set up button listeners
        setupButtonListeners()

        updateUI()
        
        // Add spinner listener
        deviceSpinner.onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: android.widget.AdapterView<*>?, view: android.view.View?, position: Int, id: Long) {
                if (foundDevices.isNotEmpty()) {
                    val selectedDevice = foundDevices[position]
                    Log.d(TAG, "Selected device: ${selectedDevice.name} (${selectedDevice.description})")
                    Toast.makeText(this@MainActivity, "Selected: ${selectedDevice.description}", Toast.LENGTH_SHORT).show()
                    updateUI()
                }
            }
            
            override fun onNothingSelected(parent: android.widget.AdapterView<*>?) {
                // Do nothing
            }
        }
    }

    private fun initializeManagers() {
        // Initialize audio capture manager
        audioCaptureManager = AudioCaptureManager(
            onAudioData = { data, size ->
                // Send audio data to ESP32 via BLE
                if (isConnected) {
                    Log.d(TAG, "=== MAIN ACTIVITY: SENDING AUDIO DATA ===")
                    Log.d(TAG, "Data size: $size bytes")
                    val success = bleAudioManager.sendAudioData(data, size)
                    if (!success) {
                        Log.e(TAG, "=== MAIN ACTIVITY: BLE SEND FAILED ===")
                        runOnUiThread {
                            Toast.makeText(this, "BLE send failed!", Toast.LENGTH_SHORT).show()
                        }
                    } else {
                        Log.d(TAG, "=== MAIN ACTIVITY: BLE SEND SUCCESS ===")
                    }
                } else {
                    Log.w(TAG, "Not connected - cannot send audio data")
                }
            },
            onError = { error ->
                Log.e(TAG, "Audio capture error: $error")
                runOnUiThread {
                    statusText.text = "Audio Error: $error"
                }
            }
        )

        // Initialize BLE audio manager
        bleAudioManager = BLEAudioManager(
            context = this,
            onConnectionStateChanged = { connected ->
                Log.d(TAG, "Connection state changed: $connected")
                isConnected = connected
                runOnUiThread {
                    updateUI()
                    if (connected) {
                        statusText.text = "Connected to ESP32"
                        Toast.makeText(this, "Connected to ESP32!", Toast.LENGTH_SHORT).show()
                    } else {
                        statusText.text = "Disconnected from ESP32"
                        Toast.makeText(this, "Disconnected from ESP32", Toast.LENGTH_SHORT).show()
                    }
                }
            },
            onDeviceFound = { device ->
                Log.d(TAG, "Device found: ${device.name} (${device.address})")
                runOnUiThread {
                    addDeviceToList(device)
                }
            },
            onError = { error ->
                Log.e(TAG, "BLE error: $error")
                runOnUiThread {
                    statusText.text = "BLE Error: $error"
                    Toast.makeText(this, "BLE Error: $error", Toast.LENGTH_LONG).show()
                }
            },
            onAudioDataReceived = { data, size ->
                Log.d(TAG, "=== MAIN ACTIVITY: AUDIO DATA RECEIVED ===")
                Log.d(TAG, "Received audio data: $size bytes")
                Log.d(TAG, "First 8 bytes: ${data.take(8).joinToString { "0x%02X".format(it) }}")
                
                // Play the received audio data
                try {
                    audioCaptureManager.playReceivedAudio(data, size)
                    Log.d(TAG, "=== MAIN ACTIVITY: AUDIO PLAYBACK INITIATED ===")
                } catch (e: Exception) {
                    Log.e(TAG, "Error calling playReceivedAudio", e)
                }
                
                runOnUiThread {
                    statusText.text = "Received: $size bytes"
                }
            }
        )
    }

    private fun addDeviceToList(device: BluetoothDevice) {
        val deviceName = device.name ?: "Unknown"
        val description = when {
            deviceName.contains("ESP32S3_Audio_Server") -> "ESP32 A (Coordinator)"
            deviceName.contains("ESP32S3_Audio_Client") -> "ESP32 B (Client)"
            else -> deviceName
        }
        
        // Check if device already exists in the list
        val existingDevice = foundDevices.find { it.device.address == device.address }
        if (existingDevice != null) {
            Log.d(TAG, "Device already exists: ${device.name} (${device.address})")
            return
        }
        
        val deviceInfo = DeviceInfo(deviceName, description, device)
        foundDevices.add(deviceInfo)
        deviceAdapter.notifyDataSetChanged()
        updateUI()
        Log.d(TAG, "Added new device: $description (${device.address})")
    }

    private fun setupButtonListeners() {
        scanButton.setOnClickListener {
            if (bleAudioManager.isScanning()) {
                stopScan()
            } else {
                startScan()
            }
        }

        connectButton.setOnClickListener {
            if (isConnected) {
                disconnect()
            } else {
                connect()
            }
        }

        recordButton.setOnClickListener {
            if (isRecording) {
                stopRecording()
            } else {
                startRecording()
            }
        }

        testButton.setOnClickListener {
            testBLEWrite()
        }
        
        // Add test tone button functionality
        recordButton.setOnLongClickListener {
            Log.d(TAG, "Long press detected - playing test tone")
            audioCaptureManager.playTestTone()
            Toast.makeText(this, "Playing test tone...", Toast.LENGTH_SHORT).show()
            true
        }
    }

    private fun startScan() {
        Log.d(TAG, "Starting scan...")
        foundDevices.clear()
        deviceAdapter.notifyDataSetChanged()
        statusText.text = "Scanning for ESP32 devices..."
        scanButton.text = "Stop Scan"
        bleAudioManager.startScan()
        updateUI()
    }

    private fun stopScan() {
        Log.d(TAG, "Stopping scan...")
        statusText.text = "Scan stopped"
        scanButton.text = "Start Scan"
        bleAudioManager.stopScan()
    }

    private fun connect() {
        if (foundDevices.isEmpty()) {
            Toast.makeText(this, "No devices found. Please scan first.", Toast.LENGTH_SHORT).show()
            return
        }

        val selectedPosition = deviceSpinner.selectedItemPosition
        Log.d(TAG, "=== CONNECTION ATTEMPT ===")
        Log.d(TAG, "Selected position: $selectedPosition")
        Log.d(TAG, "Found devices count: ${foundDevices.size}")
        
        if (selectedPosition >= 0 && selectedPosition < foundDevices.size) {
            val selectedDevice = foundDevices[selectedPosition]
            Log.d(TAG, "Connecting to: ${selectedDevice.description}")
            Log.d(TAG, "Device name: ${selectedDevice.name}")
            Log.d(TAG, "Device address: ${selectedDevice.device.address}")
            statusText.text = "Connecting to ${selectedDevice.description}..."
            connectButton.text = "Connecting..."
            bleAudioManager.connectToDevice(selectedDevice.device)
        } else {
            Log.e(TAG, "Invalid selection: position=$selectedPosition, devices=${foundDevices.size}")
            Toast.makeText(this, "Please select a device first.", Toast.LENGTH_SHORT).show()
        }
    }

    private fun disconnect() {
        Log.d(TAG, "Disconnecting...")
        statusText.text = "Disconnecting..."
        bleAudioManager.disconnect()
    }

    private fun startRecording() {
        if (!isConnected) {
            Toast.makeText(this, "Please connect to an ESP32 device first.", Toast.LENGTH_SHORT).show()
            return
        }

        Log.d(TAG, "Starting recording...")
        isRecording = true
        recordButton.text = "Stop Recording"
        statusText.text = "Recording audio..."
        audioCaptureManager.startRecording()
    }

    private fun stopRecording() {
        Log.d(TAG, "Stopping recording...")
        isRecording = false
        recordButton.text = "Start Recording"
        statusText.text = "Recording stopped"
        audioCaptureManager.stopRecording()
    }

    private fun testBLEWrite() {
        Log.d(TAG, "=== TEST BLE WRITE ATTEMPT ===")
        Log.d(TAG, "isConnected: $isConnected")
        Log.d(TAG, "bleAudioManager.isConnected(): ${bleAudioManager.isConnected()}")
        
        if (!isConnected) {
            Log.e(TAG, "Not connected to ESP32")
            Toast.makeText(this, "Please connect to an ESP32 device first.", Toast.LENGTH_SHORT).show()
            statusText.text = "Not connected - connect first"
            return
        }

        if (!bleAudioManager.isConnected()) {
            Log.e(TAG, "BLE manager reports not connected")
            Toast.makeText(this, "BLE connection lost. Please reconnect.", Toast.LENGTH_SHORT).show()
            statusText.text = "BLE connection lost"
            return
        }

        Log.d(TAG, "Testing BLE beep write...")
        statusText.text = "Testing BLE beep..."
        val success = bleAudioManager.sendTestBeep()
        
        if (success) {
            Log.d(TAG, "Beep control sent successfully")
            Toast.makeText(this, "Beep sent", Toast.LENGTH_SHORT).show()
            statusText.text = "Beep sent - check other phone"
        } else {
            Log.e(TAG, "Failed to send beep")
            Toast.makeText(this, "Failed to send beep", Toast.LENGTH_SHORT).show()
            statusText.text = "Beep failed"
        }
    }

    private fun updateUI() {
        val isScanning = bleAudioManager.isScanning()
        val hasDevices = foundDevices.isNotEmpty()
        val canConnect = hasDevices && !isConnected
        val canRecord = isConnected && !isRecording

        scanButton.text = if (isScanning) "Stop Scan" else "Start Scan"
        scanButton.isEnabled = !isConnected

        connectButton.text = when {
            isConnected -> "Disconnect"
            bleAudioManager.isConnected() -> "Disconnect"
            else -> "Connect"
        }
        connectButton.isEnabled = canConnect || isConnected

        recordButton.text = if (isRecording) "Stop Recording" else "Start Recording"
        recordButton.isEnabled = canRecord || isRecording

        testButton.isEnabled = isConnected

        deviceSpinner.isEnabled = !isConnected && hasDevices

        // Update status text
        val status = when {
            isConnected -> "Connected to ESP32"
            isScanning -> "Scanning for ESP32 devices..."
            hasDevices -> "Found ${foundDevices.size} ESP32 device(s). Select one to connect."
            else -> "Ready to scan for ESP32 devices"
        }
        statusText.text = status
    }

    private fun checkPermissions(): Boolean {
        val permissions = if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.S) {
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.RECORD_AUDIO,
                Manifest.permission.ACCESS_FINE_LOCATION
            )
        } else {
            arrayOf(
                Manifest.permission.ACCESS_FINE_LOCATION,
                Manifest.permission.RECORD_AUDIO
            )
        }

        return permissions.all { permission ->
            ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED
        }
    }

    private fun requestPermissions() {
        val permissions = if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.S) {
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.RECORD_AUDIO,
                Manifest.permission.ACCESS_FINE_LOCATION
            )
        } else {
            arrayOf(
                Manifest.permission.ACCESS_FINE_LOCATION,
                Manifest.permission.RECORD_AUDIO
            )
        }

        ActivityCompat.requestPermissions(this, permissions, REQUEST_PERMISSIONS)
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_PERMISSIONS) {
            if (grantResults.all { it == PackageManager.PERMISSION_GRANTED }) {
                Log.d(TAG, "All permissions granted")
                initializeManagers()
            } else {
                Log.w(TAG, "Some permissions were denied")
                Toast.makeText(this, "Some permissions are required for BLE audio", Toast.LENGTH_LONG).show()
            }
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQUEST_ENABLE_BT) {
            if (resultCode == RESULT_OK) {
                Log.d(TAG, "Bluetooth enabled")
                initializeManagers()
            } else {
                Log.w(TAG, "Bluetooth not enabled")
                Toast.makeText(this, "Bluetooth is required for BLE audio", Toast.LENGTH_LONG).show()
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        audioCaptureManager.stopRecording()
        bleAudioManager.disconnect()
    }
}

data class DeviceInfo(
    val name: String,
    val description: String,
    val device: BluetoothDevice
) {
    override fun toString(): String {
        return description
    }
}
