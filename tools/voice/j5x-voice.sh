#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Voice-call audio path for j5xnlte (MSM8916, q6voice). Not an RIL job: the modem DSP voice session is opened by
# the kernel when CS-Voice playback+capture PCMs are open, with the BE ports chosen by mixers beforehand.
#   j5x-voice.sh daemon           follow /data/local/tmp/j5ril-state/call_state (written by the HAL)
#   j5x-voice.sh start [route]    route: earpiece | speaker | headset   (default: file voice_route, else earpiece)
#   j5x-voice.sh stop
#   j5x-voice.sh route <route>    switch while a call is active (earpiece is the call default)
#   j5x-voice.sh mute [1|0]       microphone mute (the HAL does this on IRadioVoice.setMute)
#   j5x-voice.sh dump             print current values of every control this script touches
# Every control below comes from the kernel sources (sound/soc/qcom/qdsp6/q6voice-dai.c, sound/soc/codecs/msm8916-wcd-*.c).
# A control that tinymix does not list is reported and skipped, never silently "succeeds".
MODDIR=${0%/*}
# started from a Termux root shell: its LD_PRELOAD/PATH break /vendor/bin/sh scripts (e.g. j5x-audio-routing.sh)
unset LD_PRELOAD LD_LIBRARY_PATH
export PATH=/vendor/bin:/system/bin:/system/xbin:$PATH
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

# exact control name: the listing pads the name column, so the name is followed by 2+ blanks (or end of line)
have() { printf '%s\n' "$LIST" | grep -qE "(^|[[:space:]])$1( {2,}|[[:space:]]*\$)"; }
# mx <control> <value>: returns 1 (and logs) if the control does not exist or the write fails
mx() {
  c=
  for cand in "$1" "$1 Mux" "${1% Mux}"; do have "$cand" && { c=$cand; break; }; done
  [ -n "$c" ] || { log "MISSING control: $1"; return 1; }
  $TM -D "$CARD" set "$c" "$2" >/dev/null 2>&1 || $TM -D "$CARD" "$c" "$2" >/dev/null 2>&1 || { log "FAILED: $c = $2"; return 1; }
  log "set $c = $2"
}
# one-time list of the mixer controls the codec/q6 stack really exposes (for correcting names)
[ -f "$MODDIR/mixers.txt" ] || printf '%s\n' "$LIST" > "$MODDIR/mixers.txt" 2>/dev/null

# --- voice backend ports (must be set BEFORE the PCMs are opened) ------------------------------------------
# DT backend links: Primary MI2S RX (codec playback), Tertiary MI2S TX (codec capture)
voice_ports_on()  { mx "PRI_MI2S_RX Voice Mixer CS-Voice" 1; mx "CS-Voice Capture Mixer TERT_MI2S_TX" 1; }
voice_ports_off() { mx "PRI_MI2S_RX Voice Mixer CS-Voice" 0; mx "CS-Voice Capture Mixer TERT_MI2S_TX" 0; }

# --- codec RX: AIF1 Playback -> I2S RXn -> RXn MIX1 -> PDM_RXn -> analog -------------------------------------
# Control names as listed by tinymix on the j5xnlte (no " Mux" suffix). Backend carries I2S RX1/RX2 only, so the
# speaker amp (PDM_RX3) is fed from RX1+RX2 exactly like the stock Multimedia routing does.
# The analog switches are NOT toggled while audio runs: switching "SPK DAC Switch" off/on makes the speaker amp
# distort afterwards (seen on the device, ~60% of the time). Both analog paths stay powered (SPK DAC = On,
# EAR_S = Switch, set once before the call starts) and the route is chosen with the per-channel digital mutes:
#   earpiece: RX1 path (HPHL/HPHR DAC -> EAR PA) open, RX3 path (speaker) muted
#   speaker:  RX3 path open, RX1 path muted
route_rx() {
  case "$1" in
    earpiece) mx "RX1 Mute Switch" 0; mx "RX3 Mute Switch" 1 ;;
    speaker)  mx "RX3 Mute Switch" 0; mx "RX1 Mute Switch" 1 ;;
    headset)  mx "RX1 Mute Switch" 0; mx "RX2 Mute Switch" 0; mx "RX3 Mute Switch" 1
              mx "HPHL" Switch; mx "HPHR" Switch ;;
    *) log "unknown route $1"; return 1 ;;
  esac
}
# static analog setup, done before the PCMs are opened (nothing is playing yet, so no pops)
analog_setup() {
  mx "RX1 MIX1 INP1" RX1; mx "RDAC2 MUX" RX1
  mx "RX3 MIX1 INP1" RX1; mx "RX3 MIX1 INP2" RX2
  mx "EAR_S" Switch; mx "SPK DAC Switch" 1
  case "$1" in headset) ;; *) mx "HPHL" ZERO; mx "HPHR" ZERO ;; esac
  mx "RX2 Mute Switch" 0
}
# --- codec TX: AMICn -> ADCn -> DECn MUX -> CIC MUX(AMIC) -> I2S TX1 -> AIF1 Capture ----------------------
# DEC1 MUX / CIC1 MUX names are tried as "<name> Mux" and "<name>"; see mixers.txt if neither exists.
route_tx() {
  case "$1" in
    headset)   # AMIC2 -> ADC2 (ADC2 MUX = INP2)
      mx "ADC2 MUX" INP2; TXSRC=ADC2 ;;
    *)         # AMIC1 -> ADC1 (board has one mic, bias External1 per DT audio-routing)
      TXSRC=ADC1 ;;
  esac
  echo "$TXSRC" > "$ST/tx_src"
  mx "CIC1 MUX" AMIC
  apply_mute
}
# mic mute = TX decimator input to ZERO (enum value from the driver); written by the HAL on IRadioVoice.setMute
apply_mute() {
  m=$(cat "$ST/mic_mute" 2>/dev/null); src=$(cat "$ST/tx_src" 2>/dev/null); src=${src:-ADC1}
  if [ "$m" = 1 ]; then mx "DEC1 MUX" ZERO; else mx "DEC1 MUX" "$src"; fi
  echo "$m" > "$ST/mic_mute.cur"
}

holder_pid() { cat "$ST/hold.pid" 2>/dev/null; }
running() { p=$(holder_pid); [ -n "$p" ] && kill -0 "$p" 2>/dev/null; }

do_route() { route_rx "$1"; route_tx "$1"; echo "$1" > "$ST/voice_route.cur"; }

do_start() {
  r=${1:-$(cat "$ST/voice_route" 2>/dev/null)}; r=${r:-earpiece}
  running && { log "already running"; return 0; }
  [ -x "$HOLD" ] || { log "no $HOLD"; return 1; }
  log "start route=$r"
  voice_ports_on; analog_setup "$r"; do_route "$r"
  "$HOLD" "$CARD" "$DEV" >> "$LOG" 2>&1 &
  echo $! > "$ST/hold.pid"
  sleep 1
  running || { log "voice_hold died, see log"; voice_ports_off; return 1; }
  return 0
}
do_stop() {
  running && { kill "$(holder_pid)"; n=0; while running && [ $n -lt 20 ]; do sleep 0.2; n=$((n+1)); done; }
  rm -f "$ST/hold.pid" "$ST/mic_mute.cur"; echo 0 > "$ST/mic_mute"
  voice_ports_off
  mx "RX1 Mute Switch" 0; mx "RX2 Mute Switch" 0; mx "RX3 Mute Switch" 0
  log "stopped"
  # put Multimedia routing back the way the device normally has it
  [ -x /vendor/bin/j5x-audio-routing.sh ] && /vendor/bin/j5x-audio-routing.sh >> "$LOG" 2>&1
}

case "$1" in
  start) do_start "$2" ;;
  stop) do_stop ;;
  route) echo "$2" > "$ST/voice_route"; running && do_route "$2" ;;
  mute) echo "${2:-1}" > "$ST/mic_mute"; running && apply_mute ;;
  dump) for c in "PRI_MI2S_RX Voice Mixer CS-Voice" "CS-Voice Capture Mixer TERT_MI2S_TX" "RX1 MIX1 INP1" "RX2 MIX1 INP1" "RX3 MIX1 INP1" "RX3 MIX1 INP2" "RX1 Mute Switch" "RX2 Mute Switch" \
          "RDAC2 MUX" "EAR_S" "HPHL" "HPHR" "SPK DAC Switch" "ADC2 MUX" "DEC1 MUX" "CIC1 MUX" "RX3 Mute Switch"; do
          echo "== $c"; $TM -D "$CARD" get "$c" 2>&1 || $TM -D "$CARD" "$c" 2>&1; done ;;
  daemon)
    log "daemon up"; cur=""
    while :; do
      s=$(cat "$ST/call_state" 2>/dev/null); [ "$s" = active ] || s=idle
      if [ "$s" = active ]; then
        running || do_start
        want=$(cat "$ST/voice_route" 2>/dev/null); want=${want:-earpiece}
        [ "$want" = "$(cat "$ST/voice_route.cur" 2>/dev/null)" ] || { running && do_route "$want"; }
        [ "$(cat "$ST/mic_mute" 2>/dev/null)" = "$(cat "$ST/mic_mute.cur" 2>/dev/null)" ] || { running && apply_mute; }
      else
        running && do_stop
      fi
      sleep 1
    done ;;
  *) echo "usage: $0 daemon|start [route]|stop|route <r>|dump"; exit 2 ;;
esac
