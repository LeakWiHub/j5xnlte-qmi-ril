#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Voice-call audio path for j5xnlte (MSM8916, q6voice). Not an RIL job: the modem DSP voice session is opened by
# the kernel when CS-Voice playback+capture PCMs are open, with the BE ports chosen by mixers beforehand.
#   j5x-voice.sh daemon           follow /data/local/tmp/j5ril-state/call_state (written by the HAL)
#   j5x-voice.sh start [route]    route: earpiece | speaker | headset   (default: file voice_route, else earpiece)
#   j5x-voice.sh stop
#   j5x-voice.sh route <route>    switch while a call is active
#   j5x-voice.sh dump             print current values of every control this script touches
# Every control below comes from the kernel sources (sound/soc/qcom/qdsp6/q6voice-dai.c, sound/soc/codecs/msm8916-wcd-*.c).
# A control that tinymix does not list is reported and skipped, never silently "succeeds".
MODDIR=${0%/*}
ST=/data/local/tmp/j5ril-state
LOG=${J5X_VOICE_LOG:-$MODDIR/j5x-voice.log}
HOLD=${J5X_VOICE_HOLD:-$MODDIR/bin/voice_hold}
CARD=${J5X_CARD:-0}; DEV=${J5X_VOICE_DEV:-4}
TM=${TINYMIX:-$(command -v tinymix || command -v tinymix_stock)}
mkdir -p "$ST"
log() { echo "$(date '+%m-%d %H:%M:%S') $*" >> "$LOG"; echo "$*" >&2; }

[ -n "$TM" ] || { log "no tinymix"; exit 1; }
LIST=$({ $TM -D "$CARD" controls 2>/dev/null || $TM -D "$CARD" 2>/dev/null; } )
[ -n "$LIST" ] || LIST=$($TM 2>/dev/null)

have() { printf '%s\n' "$LIST" | grep -qF -- "$1"; }
# mx <control> <value>: returns 1 (and logs) if the control does not exist or the write fails
mx() {
  have "$1" || { log "MISSING control: $1"; return 1; }
  $TM -D "$CARD" set "$1" "$2" >/dev/null 2>&1 || $TM -D "$CARD" "$1" "$2" >/dev/null 2>&1 || { log "FAILED: $1 = $2"; return 1; }
  log "set $1 = $2"
}

# --- voice backend ports (must be set BEFORE the PCMs are opened) ------------------------------------------
# DT backend links: Primary MI2S RX (codec playback), Tertiary MI2S TX (codec capture)
voice_ports_on()  { mx "PRI_MI2S_RX Voice Mixer CS-Voice" 1; mx "CS-Voice Capture Mixer TERT_MI2S_TX" 1; }
voice_ports_off() { mx "PRI_MI2S_RX Voice Mixer CS-Voice" 0; mx "CS-Voice Capture Mixer TERT_MI2S_TX" 0; }

# --- codec RX: AIF1 Playback -> I2S RXn -> RXn MIX1 -> PDM_RXn -> analog -------------------------------------
route_rx() {
  case "$1" in
    earpiece)  # EAR PA <- HPHL DAC (PDM_RX1) and HPHR DAC (RDAC2 MUX)
      mx "RX1 MIX1 INP1 Mux" RX1; mx "RDAC2 MUX Mux" RX1
      mx "EAR_S" Switch; mx "HPHL" ZERO; mx "HPHR" ZERO; mx "SPK DAC Switch" 0 ;;
    speaker)   # SPK PA <- SPK DAC <- PDM_RX3
      mx "RX3 MIX1 INP1 Mux" RX3
      mx "SPK DAC Switch" 1; mx "EAR_S" ZERO; mx "HPHL" ZERO; mx "HPHR" ZERO ;;
    headset)   # HPHL DAC <- PDM_RX1, HPHR DAC <- RDAC2 MUX(RX2) <- PDM_RX2
      mx "RX1 MIX1 INP1 Mux" RX1; mx "RX2 MIX1 INP1 Mux" RX2; mx "RDAC2 MUX Mux" RX2
      mx "HPHL" Switch; mx "HPHR" Switch; mx "EAR_S" ZERO; mx "SPK DAC Switch" 0 ;;
    *) log "unknown route $1"; return 1 ;;
  esac
}
# --- codec TX: AMICn -> ADCn -> DECn MUX -> CIC MUX(AMIC) -> I2S TX1 -> AIF1 Capture ----------------------
route_tx() {
  case "$1" in
    headset)   # AMIC2 -> ADC2 (ADC2 MUX = INP2)
      mx "ADC2 MUX Mux" INP2; mx "DEC1 MUX Mux" ADC2 ;;
    *)         # AMIC1 -> ADC1 (board has one mic, bias External1 per DT audio-routing)
      mx "DEC1 MUX Mux" ADC1 ;;
  esac
  mx "CIC1 MUX Mux" AMIC
}

holder_pid() { cat "$ST/hold.pid" 2>/dev/null; }
running() { p=$(holder_pid); [ -n "$p" ] && kill -0 "$p" 2>/dev/null; }

do_route() { route_rx "$1"; route_tx "$1"; echo "$1" > "$ST/voice_route.cur"; }

do_start() {
  r=${1:-$(cat "$ST/voice_route" 2>/dev/null)}; r=${r:-earpiece}
  running && { log "already running"; return 0; }
  [ -x "$HOLD" ] || { log "no $HOLD"; return 1; }
  log "start route=$r"
  voice_ports_on; do_route "$r"
  "$HOLD" "$CARD" "$DEV" >> "$LOG" 2>&1 &
  echo $! > "$ST/hold.pid"
  sleep 1
  running || { log "voice_hold died, see log"; voice_ports_off; return 1; }
  return 0
}
do_stop() {
  running && { kill "$(holder_pid)"; n=0; while running && [ $n -lt 20 ]; do sleep 0.2; n=$((n+1)); done; }
  rm -f "$ST/hold.pid"
  voice_ports_off
  log "stopped"
  # put Multimedia routing back the way the device normally has it
  [ -x /vendor/bin/j5x-audio-routing.sh ] && /vendor/bin/j5x-audio-routing.sh >> "$LOG" 2>&1
}

case "$1" in
  start) do_start "$2" ;;
  stop) do_stop ;;
  route) echo "$2" > "$ST/voice_route"; running && do_route "$2" ;;
  dump) for c in "PRI_MI2S_RX Voice Mixer CS-Voice" "CS-Voice Capture Mixer TERT_MI2S_TX" "RX1 MIX1 INP1 Mux" "RX2 MIX1 INP1 Mux" "RX3 MIX1 INP1 Mux" \
          "RDAC2 MUX Mux" "EAR_S" "HPHL" "HPHR" "SPK DAC Switch" "ADC2 MUX Mux" "DEC1 MUX Mux" "CIC1 MUX Mux"; do
          echo "== $c"; $TM -D "$CARD" get "$c" 2>&1 || $TM -D "$CARD" "$c" 2>&1; done ;;
  daemon)
    log "daemon up"; cur=""
    while :; do
      s=$(cat "$ST/call_state" 2>/dev/null); [ "$s" = active ] || s=idle
      if [ "$s" = active ]; then
        running || do_start
        want=$(cat "$ST/voice_route" 2>/dev/null); want=${want:-earpiece}
        [ "$want" = "$(cat "$ST/voice_route.cur" 2>/dev/null)" ] || { running && do_route "$want"; }
      else
        running && do_stop
      fi
      sleep 1
    done ;;
  *) echo "usage: $0 daemon|start [route]|stop|route <r>|dump"; exit 2 ;;
esac
