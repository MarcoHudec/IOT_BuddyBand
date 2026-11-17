#!/usr/bin/env bash
# Flashes the boards one after another: names the board to plug in, waits for Enter,
# finds it by its MAC among all connected boards (docs/protocol.md §8) and flashes it.
#   tools/flash_by_mac.sh                    # band1 band2 band3 teacher
#   tools/flash_by_mac.sh band1 band3        # only these, in this order
set -u
cd "$(dirname "$0")/../firmware"
PIO=~/.platformio/penv/bin/pio
ESPTOOL=~/.platformio/packages/tool-esptool/esptool
declare -A MAC_OF=(
  [teacher]=40:f5:20:70:58:90
  [band1]=24:0a:c4:1d:25:f8
  [band2]=24:0a:c4:1d:24:70
  [band3]=40:f5:20:70:15:70
)
LABEL_teacher="Lehrer-Band (Board 0)"; LABEL_band1="Kind-Band 1"; LABEL_band2="Kind-Band 2"; LABEL_band3="Kind-Band 3"
envs=("$@"); ((${#envs[@]})) || envs=(band1 band2 band3 teacher)

find_port() {   # prints the port whose board has MAC $1
  local p
  for p in /dev/ttyUSB* /dev/ttyACM*; do
    [[ -e $p ]] || continue
    if "$ESPTOOL" --port "$p" read-mac 2>/dev/null | grep -qi "MAC: *$1"; then echo "$p"; return 0; fi
  done
  return 1
}

done_list=()
for env in "${envs[@]}"; do
  label_var="LABEL_$env"; label=${!label_var:-$env}; mac=${MAC_OF[$env]:-}
  [[ -n $mac ]] || { echo "!!! unknown target $env"; continue; }
  while :; do
    echo
    echo "================================================================"
    echo "  Next: $label   (MAC $mac)"
    echo "  Plug it in via USB and press Enter  (s = skip, q = quit)"
    echo "================================================================"
    read -r answer
    [[ $answer == q ]] && exit 0
    [[ $answer == s ]] && break
    echo "... looking for $label on the connected boards"
    if ! port=$(find_port "$mac"); then
      echo "!!! $label not found. Connected: $(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | tr '\n' ' ')"
      echo "    Check the cable (some USB cables only charge) and try again."
      continue
    fi
    echo ">>> found on $port, flashing $env ..."
    if "$PIO" run -e "$env" -t upload --upload-port "$port" 2>&1 | grep -E "error|SUCCESS|FAILED" | tail -3 | grep -q SUCCESS; then
      echo "=== $label done. You can unplug it."
      done_list+=("$env")
      break
    else
      echo "!!! flashing failed – press Enter to try again (hold BOOT on the board if it keeps failing)"
    fi
  done
done
echo
echo "=== finished: ${done_list[*]:-nothing}"
