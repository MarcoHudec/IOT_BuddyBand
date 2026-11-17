#include "ble_link.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>

namespace bb {
namespace {

constexpr UBaseType_t RX_QUEUE_LEN = 64;  // ~6 s of traffic from 1 neighbour at 10/s

QueueHandle_t      rxQueue     = nullptr;
NimBLEAdvertising* advertising = nullptr;
std::atomic<uint32_t> dropped{0};  // written by NimBLE task, read by loop()

// Runs in the NimBLE host task, not in loop(): only parse and hand over via the queue.
class ScanHandler : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (!dev->haveManufacturerData()) return;
    std::string mfg = dev->getManufacturerData();
    if (mfg.size() < 2 + sizeof(Header)) return;

    const uint8_t* raw = reinterpret_cast<const uint8_t*>(mfg.data());
    uint16_t company = raw[0] | (raw[1] << 8);
    if (company != COMPANY_ID) return;

    RxPacket rx;
    if (!parsePacket(raw + 2, mfg.size() - 2, GROUP_ID, rx.pkt)) return;
    rx.rssi = static_cast<int8_t>(dev->getRSSI());
    rx.ms = millis();
    if (xQueueSend(rxQueue, &rx, 0) != pdTRUE) dropped++;
  }
};

ScanHandler scanHandler;

}  // namespace

void bleBegin(int8_t txPowerDbm) {
  rxQueue = xQueueCreate(RX_QUEUE_LEN, sizeof(RxPacket));

  NimBLEDevice::init("");
  NimBLEDevice::setPower(txPowerDbm);  // keep identical on all boards (RSSI comparability)

  advertising = NimBLEDevice::getAdvertising();
  advertising->setConnectableMode(BLE_GAP_CONN_MODE_NON);
  advertising->setMinInterval(160);  // 160 * 0.625 ms = 100 ms
  advertising->setMaxInterval(160);

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanHandler, true);  // true = report every advertisement (duplicates)
  scan->setActiveScan(false);                  // passive: we never send scan requests
  scan->setInterval(100);
  scan->setWindow(100);                        // window == interval -> 100 % listening
  scan->setDuplicateFilter(false);
  scan->start(0, false);                       // 0 = scan forever
}

void bleAdvertise(const Packet& p) {
  uint8_t mfg[2 + MAX_PACKET];
  mfg[0] = COMPANY_ID & 0xFF;
  mfg[1] = COMPANY_ID >> 8;
  memcpy(mfg + 2, p.data, p.len);

  NimBLEAdvertisementData data;
  data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  data.setManufacturerData(mfg, 2 + p.len);

  advertising->stop();
  advertising->setAdvertisementData(data);
  advertising->start();
}

bool bleReceive(RxPacket& out) {
  return rxQueue && xQueueReceive(rxQueue, &out, 0) == pdTRUE;
}

uint32_t bleDropped() { return dropped; }

}  // namespace bb
