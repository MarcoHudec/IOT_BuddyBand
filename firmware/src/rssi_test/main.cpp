// RSSI measurement for milestone M1.
//
// Every board advertises a STATUS packet once per second and scans all the
// time. Each received BuddyBand packet is printed as one CSV line:
//
//   ms,self,tx,seq,rssi
//
// Procedure: place two boards at 1/5/10/20/30 m, collect ~30 lines per
// distance, save as evaluation/rssi/<environment>_<distance>m.csv.

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "protocol.h"

#ifndef NODE_ID
#error "NODE_ID must be set in platformio.ini"
#endif

static uint16_t seq = 0;
static NimBLEAdvertising* advertising = nullptr;

// Puts our packet into the manufacturer data of the advertisement.
static void advertise(const bb::Packet& p) {
  uint8_t mfg[2 + bb::MAX_PACKET];
  mfg[0] = bb::COMPANY_ID & 0xFF;
  mfg[1] = bb::COMPANY_ID >> 8;
  memcpy(mfg + 2, p.data, p.len);

  NimBLEAdvertisementData data;
  data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  data.setManufacturerData(mfg, 2 + p.len);

  advertising->stop();
  advertising->setAdvertisementData(data);
  advertising->start();
}

class ScanHandler : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (!dev->haveManufacturerData()) return;
    std::string mfg = dev->getManufacturerData();
    if (mfg.size() < 2 + sizeof(bb::Header)) return;

    const uint8_t* raw = reinterpret_cast<const uint8_t*>(mfg.data());
    uint16_t company = raw[0] | (raw[1] << 8);
    if (company != bb::COMPANY_ID) return;

    bb::Packet p;
    if (!bb::parsePacket(raw + 2, mfg.size() - 2, GROUP_ID, p)) return;

    Serial.printf("%lu,%d,%d,%u,%d\n", millis(), NODE_ID, p.header().tx,
                  p.header().seq, dev->getRSSI());
  }
};

static ScanHandler scanHandler;

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.printf("# BuddyBand RSSI test, node %d, group %d\n", NODE_ID, GROUP_ID);
  Serial.println("ms,self,tx,seq,rssi");

  NimBLEDevice::init("");
  NimBLEDevice::setPower(9);  // dBm; keep identical on all boards for comparable data

  advertising = NimBLEDevice::getAdvertising();
  advertising->setConnectableMode(BLE_GAP_CONN_MODE_NON);
  advertising->setMinInterval(160);  // 160 * 0.625 ms = 100 ms
  advertising->setMaxInterval(160);

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanHandler, true);  // true = report duplicates
  scan->setActiveScan(false);
  scan->setInterval(100);
  scan->setWindow(100);
  scan->setDuplicateFilter(false);
  scan->start(0, false);  // 0 = scan forever
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last >= 1000) {
    last = millis();
    bb::StatusPayload s{bb::STAGE_COMPLETE, 0, 0, 255};
    bb::Packet p;
    bb::buildPacket(p, GROUP_ID, bb::MSG_STATUS, NODE_ID, seq++, s);
    advertise(p);
  }
}
