// SPDX-License-Identifier: Apache-2.0
// Port of tools/voice/j5x-voice.sh into the HAL. Mixer access goes through raw ALSA control ioctls (no tinymix).
// Control names/values are the ones seen with tinymix on the j5xnlte; a control that is missing or refuses the write
// is logged and skipped, never reported as success.
#define LOG_TAG "j5xnlte-voice"
#include "voice_audio.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sound/asound.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace j5ril {
namespace {

constexpr int kCard = 0, kVoiceDev = 4;
constexpr const char* kState = "/data/local/tmp/j5ril-state";

/* ---------------------------------------------------------------- mixer */
class Mixer {
public:
    bool open() {
        if (fd_ >= 0) return true;
        char p[48];
        snprintf(p, sizeof(p), "/dev/snd/controlC%d", kCard);
        fd_ = ::open(p, O_RDWR | O_CLOEXEC);
        if (fd_ < 0) { LOGW("open %s: %s", p, strerror(errno)); return false; }
        return load();
    }
    void close() { if (fd_ >= 0) ::close(fd_); fd_ = -1; list_.clear(); }

    /* base name is tried as is, with " Mux" appended and with " Mux" removed (names differ between kernels) */
    bool set(const std::string& base, const std::string& val) {
        unsigned id = 0;
        std::string cands[3] = {base, base + " Mux", base};
        if (base.size() > 4 && base.compare(base.size() - 4, 4, " Mux") == 0) cands[2] = base.substr(0, base.size() - 4);
        for (auto& c : cands) { id = find(c); if (id) break; }
        if (!id) { LOGW("MISSING control: %s", base.c_str()); return false; }
        snd_ctl_elem_info info;
        memset(&info, 0, sizeof(info));
        info.id.numid = id;
        if (ioctl(fd_, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) { LOGW("INFO %s: %s", base.c_str(), strerror(errno)); return false; }
        snd_ctl_elem_value v;
        memset(&v, 0, sizeof(v));
        v.id.numid = id;
        switch (info.type) {
        case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
        case SNDRV_CTL_ELEM_TYPE_INTEGER: {
            long x = strtol(val.c_str(), nullptr, 0);
            for (unsigned c = 0; c < info.count && c < 128; c++) v.value.integer.value[c] = x;
            break;
        }
        case SNDRV_CTL_ELEM_TYPE_ENUMERATED: {
            int item = -1;
            for (unsigned i = 0; i < info.value.enumerated.items; i++) {
                snd_ctl_elem_info t;
                memset(&t, 0, sizeof(t));
                t.id.numid = id;
                t.value.enumerated.item = i;
                if (ioctl(fd_, SNDRV_CTL_IOCTL_ELEM_INFO, &t) < 0) continue;
                if (val == t.value.enumerated.name) { item = (int)i; break; }
            }
            if (item < 0) { LOGW("FAILED: %s has no item '%s'", base.c_str(), val.c_str()); return false; }
            for (unsigned c = 0; c < info.count && c < 128; c++) v.value.enumerated.item[c] = item;
            break;
        }
        default: LOGW("FAILED: %s: unsupported control type %d", base.c_str(), (int)info.type); return false;
        }
        if (ioctl(fd_, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) < 0) { LOGW("FAILED: %s = %s: %s", base.c_str(), val.c_str(), strerror(errno)); return false; }
        LOGI("set %s = %s", base.c_str(), val.c_str());
        return true;
    }

private:
    struct E { unsigned numid; std::string name; };
    int fd_ = -1;
    std::vector<E> list_;
    unsigned find(const std::string& n) const {
        for (auto& e : list_) if (e.name == n) return e.numid;
        return 0;
    }
    bool load() {
        snd_ctl_elem_list l;
        memset(&l, 0, sizeof(l));
        if (ioctl(fd_, SNDRV_CTL_IOCTL_ELEM_LIST, &l) < 0) { LOGW("ELEM_LIST: %s", strerror(errno)); return false; }
        std::vector<snd_ctl_elem_id> ids(l.count);
        l.space = l.count;
        l.pids = ids.data();
        if (ioctl(fd_, SNDRV_CTL_IOCTL_ELEM_LIST, &l) < 0) { LOGW("ELEM_LIST: %s", strerror(errno)); return false; }
        for (unsigned i = 0; i < l.used && i < ids.size(); i++)
            list_.push_back({ids[i].numid, std::string(reinterpret_cast<char*>(ids[i].name))});
        LOGI("mixer: %zu controls", list_.size());
        return !list_.empty();
    }
};

/* ---------------------------------------------------------------- CS-Voice PCMs (was voice_hold) */
void anyParams(snd_pcm_hw_params* p) {
    memset(p, 0, sizeof(*p));
    for (int i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; i++) memset(&p->masks[i], 0xff, sizeof(p->masks[i]));
    for (int i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i++) { p->intervals[i].min = 0; p->intervals[i].max = UINT_MAX; }
    p->rmask = ~0U;
    p->info = ~0U;
}
void setMask(snd_pcm_hw_params* p, int param, unsigned val) {
    snd_mask* m = &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m, 0, sizeof(*m));
    m->bits[val >> 5] = 1U << (val & 31);
}
void setInt(snd_pcm_hw_params* p, int param, unsigned val) {
    snd_interval* i = &p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    i->min = i->max = val;
    i->integer = 1;
}
int openStream(char dir) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/snd/pcmC%dD%d%c", kCard, kVoiceDev, dir);
    int fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) { LOGW("open %s: %s", path, strerror(errno)); return -1; }
    snd_pcm_hw_params hw;
    anyParams(&hw);
    setMask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    setMask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    setMask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    setInt(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 1);
    setInt(&hw, SNDRV_PCM_HW_PARAM_RATE, 8000);
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0) { LOGW("%s HW_PARAMS: %s", path, strerror(errno)); ::close(fd); return -1; }
    snd_pcm_sw_params sw;
    memset(&sw, 0, sizeof(sw));
    sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step = 1;
    sw.avail_min = 1;
    sw.start_threshold = 1;
    sw.stop_threshold = ULONG_MAX;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) { LOGW("%s SW_PARAMS: %s", path, strerror(errno)); ::close(fd); return -1; }
    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) { LOGW("%s PREPARE: %s", path, strerror(errno)); ::close(fd); return -1; }
    return fd;
}

/* ---------------------------------------------------------------- controller */
std::mutex g_mu;
std::condition_variable g_cv;
bool g_call = false;
std::atomic<int> g_mute{0};
std::atomic<bool> g_started{false};

std::string readWord(const char* name, const char* def) {
    char p[96], b[32] = {0};
    snprintf(p, sizeof(p), "%s/%s", kState, name);
    FILE* f = fopen(p, "r");
    if (!f) return def;
    if (!fgets(b, sizeof(b), f)) b[0] = 0;
    fclose(f);
    std::string s = b;
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s.empty() ? def : s;
}

/* The stock audio HAL logs every AudioManager.setMicrophoneMute as "AHAL_Module: setMicMute: default: 0|1". */
void muteFollower() {
    for (;;) {
        char since[40];
        time_t t = time(nullptr);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(since, sizeof(since), "%m-%d %H:%M:%S.000", &tm);
        std::string cmd = std::string("exec /system/bin/logcat -T '") + since + "' -v brief 'AHAL_Module:D' '*:S' 2>/dev/null";
        FILE* p = popen(cmd.c_str(), "r");
        if (p) {
            char line[512];
            while (fgets(line, sizeof(line), p)) {
                int m = -1;
                if (strstr(line, "setMicMute: default: 1")) m = 1;
                else if (strstr(line, "setMicMute: default: 0")) m = 0;
                if (m >= 0) { g_mute = m; g_cv.notify_all(); }
            }
            pclose(p);
        }
        sleep(5);
    }
}

int streamVoiceIndex() {
    FILE* p = popen("exec /system/bin/dumpsys audio 2>/dev/null", "r");
    if (!p) return -1;
    char line[512];
    bool in = false;
    int idx = -1;
    while (fgets(line, sizeof(line), p)) {
        if (!in) { in = strncmp(line, "- STREAM_VOICE_CALL:", 20) == 0; continue; }
        const char* s = strstr(line, "streamVolume:");
        if (s) { idx = atoi(s + 13); break; }
    }
    pclose(p);   /* reads on to EOF are not needed, dumpsys gets SIGPIPE */
    return idx;
}

void restoreStockRouting() {
    pid_t pid = fork();
    if (pid == 0) {
        char* const envp[] = {const_cast<char*>("PATH=/vendor/bin:/system/bin"), nullptr};
        execle("/vendor/bin/sh", "sh", "/vendor/bin/j5x-audio-routing.sh", (char*)nullptr, envp);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, nullptr, 0);
}

class Session {
public:
    bool start() {
        if (!mx_.open()) return false;
        route_ = readWord("voice_route", "earpiece");   /* earpiece | speaker | headset */
        LOGI("call start, route=%s", route_.c_str());
        /* backend ports first, then the analog path, then the PCMs (the DSP session starts when both are open) */
        mx_.set("PRI_MI2S_RX Voice Mixer CS-Voice", "1");
        mx_.set("CS-Voice Capture Mixer TERT_MI2S_TX", "1");
        mx_.set("RX1 MIX1 INP1", "RX1"); mx_.set("RDAC2 MUX", "RX1");
        mx_.set("RX3 MIX1 INP1", "RX1"); mx_.set("RX3 MIX1 INP2", "RX2");
        mx_.set("EAR_S", "Switch"); mx_.set("SPK DAC Switch", "1");   /* never toggled during a call: it makes the amp distort */
        if (route_ != "headset") { mx_.set("HPHL", "ZERO"); mx_.set("HPHR", "ZERO"); }
        mx_.set("RX2 Mute Switch", "0");
        routeRx();
        txSrc_ = route_ == "headset" ? "ADC2" : "ADC1";
        if (route_ == "headset") mx_.set("ADC2 MUX", "INP2");
        mx_.set("CIC1 MUX", "AMIC");
        applyMute(true);
        cp_ = openStream('c');
        pb_ = cp_ < 0 ? -1 : openStream('p');
        if (cp_ < 0 || pb_ < 0) { LOGW("voice PCMs did not open, no voice session"); stop(); return false; }
        if (ioctl(cp_, SNDRV_PCM_IOCTL_START) < 0) LOGW("capture START: %s", strerror(errno));
        if (ioctl(pb_, SNDRV_PCM_IOCTL_START) < 0) LOGW("playback START: %s", strerror(errno));
        LOGI("voice session up");
        up_ = true;
        return true;
    }
    void stop() {
        if (cp_ >= 0) ::close(cp_);
        if (pb_ >= 0) ::close(pb_);
        cp_ = pb_ = -1;
        mx_.set("PRI_MI2S_RX Voice Mixer CS-Voice", "0");
        mx_.set("CS-Voice Capture Mixer TERT_MI2S_TX", "0");
        mx_.set("RX1 Mute Switch", "0"); mx_.set("RX2 Mute Switch", "0"); mx_.set("RX3 Mute Switch", "0");
        mx_.set("RX1 Digital Volume", "83"); mx_.set("RX2 Digital Volume", "84"); mx_.set("RX3 Digital Volume", "81");
        mx_.close();
        restoreStockRouting();
        up_ = false;
        curMute_ = -1; curVol_ = -1;
        LOGI("voice session stopped");
    }
    bool up() const { return up_; }
    void applyMute(bool force = false) {
        int m = g_mute.load();
        if (!force && m == curMute_) return;
        mx_.set("DEC1 MUX", m ? "ZERO" : txSrc_);
        curMute_ = m;
    }
    /* STREAM_VOICE_CALL index (1..5 seen) -> RX digital volume (0..124, stock 81..84) */
    void applyVolume() {
        int i = streamVoiceIndex();
        if (i < 0) { if (!volWarned_) { LOGW("volume: cannot read STREAM_VOICE_CALL from dumpsys"); volWarned_ = true; } return; }
        if (i == curVol_) return;
        int v = i < 1 ? 60 : 60 + (i - 1) * 6;
        if (v > 100) v = 100;
        for (const char* c : {"RX1 Digital Volume", "RX2 Digital Volume", "RX3 Digital Volume"}) mx_.set(c, std::to_string(v));
        curVol_ = i;
        LOGI("volume index %d -> RX digital %d", i, v);
    }

private:
    /* both analog paths stay powered; the route is picked with per-channel digital mutes */
    void routeRx() {
        if (route_ == "speaker") { mx_.set("RX3 Mute Switch", "0"); mx_.set("RX1 Mute Switch", "1"); }
        else if (route_ == "headset") { mx_.set("RX1 Mute Switch", "0"); mx_.set("RX2 Mute Switch", "0"); mx_.set("RX3 Mute Switch", "1"); mx_.set("HPHL", "Switch"); mx_.set("HPHR", "Switch"); }
        else { mx_.set("RX1 Mute Switch", "0"); mx_.set("RX3 Mute Switch", "1"); }
    }
    Mixer mx_;
    std::string route_, txSrc_ = "ADC1";
    int cp_ = -1, pb_ = -1, curMute_ = -1, curVol_ = -1;
    bool up_ = false, volWarned_ = false;
};

void worker() {
    Session s;
    std::thread(muteFollower).detach();
    std::unique_lock<std::mutex> lk(g_mu);
    for (;;) {
        g_cv.wait_for(lk, std::chrono::seconds(2));
        bool call = g_call;
        if (call && !s.up()) { lk.unlock(); s.start(); lk.lock(); }
        else if (!call && s.up()) { lk.unlock(); s.stop(); lk.lock(); g_mute = 0; }
        else if (call && s.up()) { lk.unlock(); s.applyMute(); s.applyVolume(); lk.lock(); }
    }
}

}  // namespace

void voiceAudioStart() {
    if (g_started.exchange(true)) return;
    mkdir(kState, 0755);
    std::thread(worker).detach();
}
void voiceAudioCall(bool active) {
    { std::lock_guard<std::mutex> g(g_mu); if (g_call == active) return; g_call = active; }
    g_cv.notify_all();
}
void voiceAudioMute(bool muted) { g_mute = muted ? 1 : 0; g_cv.notify_all(); }

}  // namespace j5ril
