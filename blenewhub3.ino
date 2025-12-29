#include <NimBLEDevice.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h> // <-- ต้องติดตั้ง Library นี้เพิ่ม

// ================= CONFIGURATION =================
const char* ssid = "Sky_High";
const char* password = "8922589225";

const char* mqtt_server = "192.168.2.20"; 
const int mqtt_port = 1883;
const char* mqtt_topic = "train/ble/cmd";
const char* mqtt_client_id = "ESP32_Train_JSON"; 

// ใส่ชื่ออุปกรณ์ BLE Device 1-4 ให้ตรง
String configNames[4] = {
    "JG_JMC-3434",  // Slot 1
    "JG_JMC-B934",  // Slot 2
    "JG_JMC-4F34",  // Slot 3
    ""              // Slot 4
};

// ================= BLE VARIABLES =================
static NimBLEUUID SERVICE_UUID("FFF0");
static NimBLEUUID CHARACTERISTIC_UUID("FFF2");

struct DeviceSlot {
    NimBLEClient* pClient = nullptr;
    NimBLERemoteCharacteristic* pRemoteChar = nullptr;
    NimBLEAddress* pAddr = nullptr; 
    bool isConnected = false;
    String name;
};

DeviceSlot slots[4];
bool scanPhase = true;
unsigned long scanStartTime;
unsigned long lastReconnectCheck = 0;

// ================= MQTT VARIABLES =================
WiFiClient espClient;
PubSubClient client(espClient);
unsigned long lastMqttReconnectAttempt = 0;

// ================= HELPER FUNCTIONS =================

// ฟังก์ชันเชื่อมต่อ BLE
bool connectToSlot(int i) {
    if (slots[i].pAddr == nullptr) return false;

    Serial.printf("[Connect] Slot %d (%s) connecting... ", i + 1, slots[i].name.c_str());

    if (slots[i].pClient != nullptr) {
        NimBLEDevice::deleteClient(slots[i].pClient);
        slots[i].pClient = nullptr;
    }

    slots[i].pClient = NimBLEDevice::createClient();
    
    class SlotCallbacks : public NimBLEClientCallbacks {
        int _id;
    public:
        SlotCallbacks(int id) : _id(id) {}
        void onDisconnect(NimBLEClient* pClient, int reason) override {
            Serial.printf("[Slot %d] Disconnected (Reason: %d)\n", _id + 1, reason);
            slots[_id].isConnected = false;
            slots[_id].pRemoteChar = nullptr;
        }
    };
    slots[i].pClient->setClientCallbacks(new SlotCallbacks(i), true);

    if (slots[i].pClient->connect(*slots[i].pAddr)) {
        NimBLERemoteService* pSvc = slots[i].pClient->getService(SERVICE_UUID);
        if (pSvc) {
            slots[i].pRemoteChar = pSvc->getCharacteristic(CHARACTERISTIC_UUID);
            if (slots[i].pRemoteChar) {
                slots[i].isConnected = true;
                Serial.println("Success!");
                
                // Initial Stop Packet
                uint8_t packet[11] = {0x5A, 0x6B, 0x02, 0x00, 0x05, 0, 0, 0, 0, 0x01, 0};
                uint8_t cs = 0; for(int k=0; k<10; k++) cs += packet[k]; packet[10] = cs;
                slots[i].pRemoteChar->writeValue(packet, 11, true);
                
                delay(200); 
                return true;
            }
        }
    }
    Serial.println("Failed.");
    return false;
}

// ฟังก์ชันส่งคำสั่งไปยัง Device
void sendPacket(int id, uint8_t v1, uint8_t v2, uint8_t v3, uint8_t v4) {
    int idx = id - 1;
    if (idx < 0 || idx >= 4) return;

    // Auto-Reconnect on Command
    if (!slots[idx].isConnected) {
        if (slots[idx].pAddr != nullptr) {
            Serial.printf("[Auto-Fix] Slot %d offline. Reconnecting...\n", id);
            if (!connectToSlot(idx)) return;
        } else {
            return; 
        }
    }

    if (slots[idx].isConnected && slots[idx].pRemoteChar) {
        uint8_t packet[11];
        packet[0] = 0x5A; packet[1] = 0x6B; packet[2] = 0x02; packet[3] = 0x00;
        packet[4] = 0x05;
        packet[5] = v1; packet[6] = v2; packet[7] = v3; packet[8] = v4;
        packet[9] = 0x01;
        
        uint8_t cs = 0; for(int b=0; b<10; b++) cs += packet[b]; packet[10] = cs;

        if(slots[idx].pRemoteChar->writeValue(packet, 11, true)) {
            Serial.printf("[Command] To Device %d -> AB: mode=%d spd=%d | CD: mode=%d spd=%d\n", id, v1, v2, v3, v4);
        }
    }
}

// ================= MQTT CALLBACK (JSON) =================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.print("[MQTT] Received JSON: ");
    for (int i = 0; i < length; i++) Serial.print((char)payload[i]);
    Serial.println();

    // 1. สร้าง JSON Document (ขนาด 256 byte เหลือเฟือสำหรับข้อมูลชุดนี้)
    // หากใช้ ArduinoJson v6 ให้ใช้ StaticJsonDocument<256> doc;
    JsonDocument doc; 

    // 2. แปลง Payload เป็น JSON Object
    DeserializationError error = deserializeJson(doc, payload, length);

    if (error) {
        Serial.print(F("[MQTT Error] deserializeJson() failed: "));
        Serial.println(error.f_str());
        return;
    }

    // 3. ดึงค่าจาก JSON
    // Expected: {"device":1, "AB_mode":1, "AB_speed":100, "CD_mode":0, "CD_speed":0}
    
    // ตรวจสอบว่ามี Key ครบไหม (Optional แต่แนะนำ)
    if (!doc.containsKey("device")) return;

    int targetId = doc["device"];
    uint8_t ab_mode = doc["AB_mode"] | 0;
    uint8_t ab_speed = doc["AB_speed"] | 0;
    uint8_t cd_mode = doc["CD_mode"] | 0;
    uint8_t cd_speed = doc["CD_speed"] | 0;

    // 4. สั่งงาน BLE
    if (targetId == 255) {
        Serial.println("[MQTT] Broadcast Command");
        for (int i = 1; i <= 4; i++) sendPacket(i, ab_mode, ab_speed, cd_mode, cd_speed);
    } else {
        sendPacket(targetId, ab_mode, ab_speed, cd_mode, cd_speed);
    }
}

// ฟังก์ชันเชื่อมต่อ MQTT
void reconnectMQTT() {
    if (!client.connected()) {
        if (millis() - lastMqttReconnectAttempt > 5000) {
            lastMqttReconnectAttempt = millis();
            Serial.print("[MQTT] Connecting...");
            
            if (client.connect(mqtt_client_id)) {
                Serial.println("Connected!");
                client.subscribe(mqtt_topic);
            } else {
                Serial.print("Failed rc=");
                Serial.println(client.state());
            }
        }
    }
}

// ================= BLE SCAN CALLBACK =================
class MyScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
        String foundName = advertisedDevice->getName().c_str();
        foundName.trim();

        for (int i = 0; i < 4; i++) {
            if (configNames[i] != "" && configNames[i].equals(foundName)) {
                if (slots[i].pAddr == nullptr) {
                    slots[i].pAddr = new NimBLEAddress(advertisedDevice->getAddress());
                    slots[i].name = foundName;
                    Serial.printf("   [Matched] Slot %d -> %s (%s)\n", 
                                  i + 1, foundName.c_str(), slots[i].pAddr->toString().c_str());
                }
            }
        }
    }
};

// ================= MAIN SETUP =================
void setup() {
    Serial.begin(115200);
    
    // 1. Setup WiFi
    Serial.printf("\n[WiFi] Connecting to %s", ssid);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500); Serial.print(".");
    }
    Serial.println("\n[WiFi] Connected!");

    // 2. Setup MQTT
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(mqttCallback);

    // 3. Setup BLE
    NimBLEDevice::init("ESP32_Train_Hub");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    Serial.println("--- Phase 1: Initial Scan (10s) ---");
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setScanCallbacks(new MyScanCallbacks(), false);
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(99);
    pScan->start(0, false); 
    
    scanStartTime = millis();
}

// ================= MAIN LOOP =================
void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        // Handle WiFi Reconnect logic if needed
    }
    
    if (!client.connected()) {
        reconnectMQTT();
    }
    client.loop(); 

    // BLE Phase Handling
    if (scanPhase && (millis() - scanStartTime > 10000)) {
        NimBLEDevice::getScan()->stop();
        scanPhase = false;
        Serial.println("\n--- Phase 2: Connecting Phase ---");
        for(int i=0; i<4; i++) {
            if(slots[i].pAddr != nullptr) connectToSlot(i);
        }
        Serial.println("--- System Ready for JSON Command ---");
    }

    if (scanPhase) return;

    // Auto-Reconnect every 30s
    if (millis() - lastReconnectCheck > 30000) {
        for (int i = 0; i < 4; i++) {
            if (configNames[i] != "" && slots[i].pAddr != nullptr && !slots[i].isConnected) {
                Serial.printf("[Auto-Retry] Slot %d...\n", i+1);
                connectToSlot(i);
            }
        }
        lastReconnectCheck = millis();
    }
    
    // Serial Input (Backup Control) - รับเป็น CSV เหมือนเดิมเพื่อความง่าย
    if (Serial.available() > 0) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        // (Simple Serial Parsing code here same as before...)
        int v[5]; int count = 0; int lastPos = 0;
        for (int i = 0; i <= input.length() && count < 5; i++) {
            if (i == input.length() || input.charAt(i) == ',') {
                v[count++] = input.substring(lastPos, i).toInt(); lastPos = i + 1;
            }
        }
        if (count == 5) {
            if (v[0] == 255) for(int i=1;i<=4;i++) sendPacket(i, v[1], v[2], v[3], v[4]);
            else sendPacket(v[0], v[1], v[2], v[3], v[4]);
        }
    }
}