#include "ble_bridge.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLESecurity.h>
#include <BLE2902.h>
#include <Arduino.h>
#include <string.h>

// Nordic UART Service UUIDs — every BLE serial example uses these, so
// existing tools (nRF Connect, bluefy, Web Bluetooth examples) can talk to
// us without custom UUIDs.
#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// Incoming bytes are buffered in a simple ring for bleRead()/bleAvailable().
// Sized to hold a transcript snapshot JSON plus headroom; the GATT layer
// will flow-control if we fall behind.
static const size_t RX_CAP = 2048;
static uint8_t  rxBuf[RX_CAP];
static volatile size_t rxHead = 0;
static volatile size_t rxTail = 0;

static BLEServer*         server = nullptr;
static BLECharacteristic* txChar = nullptr;
static BLECharacteristic* rxChar = nullptr;
static volatile bool      connected = false;
static volatile bool      secure = false;
static volatile uint32_t  passkey = 0;
static volatile uint16_t  mtu = 23;

static void rxPush(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    size_t next = (rxHead + 1) % RX_CAP;
    if (next == rxTail) return;  // full — drop (upstream should keep up)
    rxBuf[rxHead] = p[i];
    rxHead = next;
  }
}

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    // Arduino-ESP32 3.x returns String here, not std::string as 2.x did.
    String v = c->getValue();
    if (v.length()) rxPush((const uint8_t*)v.c_str(), v.length());
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    connected = true;
    Serial.println("[ble] connected");
  }
  void onDisconnect(BLEServer* s) override {
    onDisconnect(s, nullptr);
  }
  // The reason code is the difference between "the host gave up on
  // encryption" and "the link just ended", which are not otherwise
  // distinguishable from the outside. 0x13/0x16 are remote/local user
  // termination; 0x3D is MIC failure, i.e. a stale bond key.
  void onDisconnect(BLEServer* s, esp_ble_gatts_cb_param_t* param) override {
    bool wasSecure = secure;
    connected = false;
    secure = false;
    passkey = 0;
    mtu = 23;
    if (param) {
      Serial.printf("[ble] disconnected reason=0x%02X secure_was=%d bonds=%d\n",
                    param->disconnect.reason, wasSecure ? 1 : 0,
                    esp_ble_get_bond_device_num());
    } else {
      Serial.printf("[ble] disconnected (no reason) secure_was=%d bonds=%d\n",
                    wasSecure ? 1 : 0, esp_ble_get_bond_device_num());
    }
    // Restart advertising so the next client can find us.
    BLEDevice::startAdvertising();
  }
  void onMtuChanged(BLEServer*, esp_ble_gatts_cb_param_t* param) override {
    mtu = param->mtu.mtu;
    Serial.printf("[ble] mtu=%u\n", mtu);
  }
};

// LE Secure Connections, passkey-entry: we are DisplayOnly, the central
// is KeyboardOnly. The stack picks a random 6-digit passkey, calls
// onPassKeyNotify here, and the user types it on the desktop. main.cpp
// polls blePasskey() to render it.
class SecCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  bool onConfirmPIN(uint32_t) override { return false; }
  bool onSecurityRequest() override { return true; }
  // Not called under Just Works. Kept so switching the auth mode back to
  // ESP_LE_AUTH_REQ_SC_MITM_BOND restores the passkey screen with no other
  // changes; main.cpp still polls blePasskey() and renders it.
  void onPassKeyNotify(uint32_t pk) override {
    passkey = pk;
    Serial.printf("[ble] passkey %06lu\n", (unsigned long)pk);
  }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
    passkey = 0;
    secure = cmpl.success;
    Serial.printf("[ble] auth %s\n", cmpl.success ? "ok" : "FAIL");
    if (!cmpl.success && server) server->disconnect(server->getConnId());
  }
};

void bleInit(const char* deviceName) {
  BLEDevice::init(deviceName);
  // Request the biggest MTU we can get. macOS negotiates to 185 typically.
  BLEDevice::setMTU(517);

  // Just Works bonding rather than passkey-with-MITM.
  //
  // REFERENCE.md recommends DisplayOnly + a 6-digit passkey, and upstream
  // does that. In practice the bond desynced repeatedly against macOS,
  // which persists BLE bonds to disk and then refuses to re-pair a device
  // it believes it knows - the link dies before authentication (HCI reason
  // 0x13) and the only way back was the reset menu plus reading six digits
  // off the screen.
  //
  // Just Works keeps the link AES-CCM encrypted and still bonds, so
  // reconnects reuse the stored key; it drops only MITM protection during
  // the pairing handshake itself. The trade is deliberate: an attacker
  // would have to be in radio range at the exact moment of pairing,
  // whereas a desync that needs manual recovery happened repeatedly.
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
  BLEDevice::setSecurityCallbacks(new SecCallbacks());

  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService* svc = server->createService(NUS_SERVICE_UUID);

  txChar = svc->createCharacteristic(
    NUS_TX_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  txChar->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
  BLE2902* cccd = new BLE2902();
  cccd->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);
  txChar->addDescriptor(cccd);

  rxChar = svc->createCharacteristic(
    NUS_RX_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  rxChar->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
  rxChar->setCallbacks(new RxCallbacks());

  svc->start();

  BLESecurity* sec = new BLESecurity();
  sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);   // no MITM
  sec->setCapability(ESP_IO_CAP_NONE);                   // no display = Just Works
  sec->setKeySize(16);
  sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  sec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);   // iOS-friendly connection interval
  adv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
  // Ground truth for what is on air, rather than what we asked for.
  Serial.printf("[ble] advertising as '%s' addr=%s\n",
                deviceName, BLEDevice::getAddress().toString().c_str());
}

// Periodic state line, so a random disconnect can be classified after the
// fact: a continuous uptime means the link dropped, a reset one means the
// device rebooted, and the bond count says whether our key survived.
void bleLogState() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 30000) return;
  last = now;
  Serial.printf("[state] up=%lus conn=%d sec=%d bonds=%d heap=%u\n",
                (unsigned long)(now / 1000), connected ? 1 : 0,
                secure ? 1 : 0, esp_ble_get_bond_device_num(),
                (unsigned)ESP.getFreeHeap());
}

bool bleConnected() { return connected; }
bool bleSecure()    { return secure; }
uint32_t blePasskey() { return passkey; }

void bleClearBonds() {
  int n = esp_ble_get_bond_device_num();
  if (n <= 0) return;
  esp_ble_bond_dev_t* list = (esp_ble_bond_dev_t*)malloc(n * sizeof(esp_ble_bond_dev_t));
  if (!list) return;
  esp_ble_get_bond_device_list(&n, list);
  for (int i = 0; i < n; i++) esp_ble_remove_bond_device(list[i].bd_addr);
  free(list);
  Serial.printf("[ble] cleared %d bond(s)\n", n);
}

size_t bleAvailable() {
  return (rxHead + RX_CAP - rxTail) % RX_CAP;
}

int bleRead() {
  if (rxHead == rxTail) return -1;
  int b = rxBuf[rxTail];
  rxTail = (rxTail + 1) % RX_CAP;
  return b;
}

size_t bleWrite(const uint8_t* data, size_t len) {
  if (!connected || !txChar) return 0;
  // ATT notify payload is limited to (MTU - 3). macOS negotiates 185, so
  // the 182-byte chunk works there; use the live mtu so a peer that caps
  // at the 23-byte default doesn't get truncated notifies.
  size_t chunk = mtu > 3 ? mtu - 3 : 20;
  if (chunk > 180) chunk = 180;
  size_t sent = 0;
  while (sent < len) {
    size_t n = len - sent;
    if (n > chunk) n = chunk;
    txChar->setValue((uint8_t*)(data + sent), n);
    txChar->notify();
    sent += n;
    // Small yield so the BLE stack flushes before the next chunk. Only
    // between chunks: delaying after the last one just adds latency, and
    // during a folder push every ack is a single chunk, so that delay was
    // pure cost on the critical path.
    if (sent < len) delay(4);
  }
  return sent;
}
