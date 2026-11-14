#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
# One-shot dump for SMS / data / registration problems -> /sdcard/j5-diag.txt
unset LD_PRELOAD LD_LIBRARY_PATH
export PATH=/vendor/bin:/system/bin:/system/xbin:$PATH
MODDIR=${0%/*}; OUT=${J5_OUT:-/sdcard/j5-diag.txt}
{
echo "### date"; date
echo "### HAL log (whole j5ril.log, last 200)"; tail -200 "$MODDIR/j5ril.log"
echo "### logcat j5xnlte-ril"; logcat -d -s j5xnlte-ril:V | tail -150
echo "### registration (telephony.registry)"; dumpsys telephony.registry | grep -iE "mServiceState|mVoiceRegState|mDataRegState|mDataConnectionState|mCallState" | head -12
echo "### framework SMS lines"; logcat -d | grep -iE "newSms|InboundSms|SmsDispatcher|WapPush" | tail -30
echo "### net"; ip addr show wwan0; ip route; ip rule; grep wwan0 /proc/net/dev
echo "### connectivity"; dumpsys connectivity | grep -iE "wwan|Mobile|validat|NetworkAgent" | head -30
echo "### dmesg bam/rmnet/wwan"; dmesg | grep -iE "bam|dmux|rmnet|wwan|q6voice|afe" | tail -60
} > "$OUT" 2>&1
echo "written $OUT"
