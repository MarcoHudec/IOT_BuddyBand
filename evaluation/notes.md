# Measurement notes

One entry per run: date, place, environment, boards, TX power, setup, result, who.

## 2026-11-14 – indoor RSSI, apartment (Milorad)

| File | Setup | Result |
| --- | --- | --- |
| `rssi/indoor_5m.csv` | sender board 1 (`rssi_a`, ID 11) on power bank, receiver board 3 on laptop, TX 9 dBm; board 2 still on (filtered) | median −61 dBm, IQR −67…−53; people walking around during the run |
| `rssi/walk_indoor_1.csv` | `live_rssi.py` A=−47, n=3 (guessed); approach to 0 m, walk away, 30 s standing at ~4 m | median filter suppresses −100 dBm dropout; saturation ≈ −20 dBm when boards touch; standing still ±1.5 dB |

## 2026-11-14 – mesh, indoor (Milorad)

| File / source | Setup | Result |
| --- | --- | --- |
| `mesh/multihop_indoor_1.log` | 3 bands in one room, board 2 observer; `ignore 3`, then `ignore 1`, then `clear` | with tx=3 ignored all 10 STATUS of band 3 arrived via band 1 (`hops=1`, seq gap-free); isolated after 8.1 s; recovery critical +0.8 s, complete +3.2 s |
| serial monitor (not saved) | indoor through walls, all bands held in hand; board 2 on laptop, bands 1 + 3 carried away; distances not recorded | direct link to band 3 lost at median ≈ −93 dBm (single packets down to −100); band 3 stayed reachable via band 1 (`hops=1`) for ~45 s and its stage "critical" was visible at board 2; board 2: complete→drifting (best −85), →critical (−94), →isolated after 8 s; recovery isolated→critical→drifting→complete. No queue drops. Band 3 restart at 58 s was a manual unplug |

## 2026-11-14 – alarms + ACK, indoor (Milorad)

`mesh/alarm_test_1.log` – teacher band (board 0) on laptop, bands 1–3 on power banks, 300 s.

| Phase | Action | Teacher saw |
| --- | --- | --- |
| 2 | band 2 unplugged ~12 s | `band 2 isolated` after 8.5 s, `reachable again` 12 s later |
| 3 | band 3 carried away through walls, back | `complete -> drifting` (ACK), teacher-side `band 3 isolated` after 8 s; on return `reachable again`, then `isolated -> critical`, `-> drifting`, `-> complete`, each ACKed |
| 4 | band 1 halfway, band 3 beyond | band 1 and band 3 `-> drifting`; band 3's alarm arrived via band 1 (`hops=1`) and was sent twice 0.9 s apart (first ACK did not arrive within the 1 s retry over 2 hops) – second copy ACKed |

All 9 alarms in the log were ACKed (`alarms rx=11 acks tx=11` incl. 2 before the recording), no queue drops.
Observation: alarms raised while out of range (band 3 → critical → isolated) were superseded by the newer
`isolated -> critical` on return – by design (latest state; `prev_stage` still tells the teacher it was isolated).
Retry 1 s is borderline for 2 hops → consider 1.5 s.

Findings: reception floor ≈ −95 dBm; provisional T_near −70 too strict indoors relative to
actual link loss; distances must be recorded in the outdoor calibration.
