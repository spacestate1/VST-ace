/* pestudio -- standalone Qt6 front end for the native Windows-VST2 loader.
 *
 * Browses the plugin directory, loads a DLL through pehost (no Wine), and plays
 * it live: programs, every exposed parameter, and a playable keyboard.
 *
 * Audio runs on PipeWire's own data-loop, which RTKit has already granted
 * realtime priority, so a small quantum does not underrun. The Qt thread never
 * touches the plugin during render -- notes and parameter writes go through
 * pehost's lock-free queue, and swapping plugins parks the stream first. */

#include <QtWidgets>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <thread>
#include <vector>

#include "midiio.h"

extern "C" {
#include "pehost.h"
#include "version.h"
#include "patch.h"
#include "win32host.h"
#include "vst3.h"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
}

/* macOS hosting is switched off in the UI for now.
 *
 * Nothing is removed: machoload.c, the Objective-C runtime, the software Metal
 * rasteriser, macvst and macau are all still compiled into pehost and still
 * work. peload will still load a .vst or .component from the command line, and
 * a patch bank naming one still opens it. This only stops the window offering
 * them -- the macOS roots are left out of the directory list and macOS bundles
 * are skipped when a directory is scanned, so there is no route to one by
 * clicking.
 *
 * Build with -DPESTUDIO_MAC=1 to put it back.
 */
#ifndef PESTUDIO_MAC
#define PESTUDIO_MAC 0
#endif

static const int    kSampleRate = 48000;
static const int    kQuantum    = 256;      /* 5.3 ms */

/* Editor zoom range. A plug-in editor is drawn at whatever size the plug-in
 * decided on, and several of them are taller than a 1080p screen -- so the
 * useful direction is out, not in. The bottom of the range is what makes a
 * 1900x1200 editor fit a window; the top is there because a small, dense editor
 * is worth a closer look, not because anything needs 4x. */
static const double kZoomMin = 0.25;
static const double kZoomMax = 4.00;

/* --------------------------------------------------------------- recorder */

/* Captures what comes out of the engine to a WAV, while it is being played.
 *
 * The audio callback may not touch a file: opening, writing and growing one all
 * block for unbounded time, and a blocked callback is a dropout. So the callback
 * only copies into a preallocated ring -- no allocation, no locks, no syscalls --
 * and a writer thread drains it to disk. Four seconds of ring is far more than
 * the writer needs and means a scheduling hiccup costs nothing.
 *
 * If the ring ever does fill, the frames are dropped and counted rather than
 * overwriting what has not been written yet: a recording with a gap in it is
 * recoverable, one whose length silently disagrees with what was played is not.
 */
class Recorder {
public:
    ~Recorder() { stop(); }

    bool start(const QString &path, int rate)
    {
        if (running_.load(std::memory_order_acquire)) return false;
        out_.open(path.toLocal8Bit().constData(), std::ios::binary | std::ios::trunc);
        if (!out_) return false;
        rate_ = rate;
        ring_.assign(size_t(rate) * 2 * kRingSeconds, 0.0f);
        head_.store(0, std::memory_order_relaxed);
        tail_ = 0;
        frames_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
        writeHeader(0);
        running_.store(true, std::memory_order_release);
        writer_ = std::thread([this] { drainLoop(); });
        path_ = path;
        return true;
    }

    /* Returns the finished file, or an empty string if nothing was recording. */
    QString stop()
    {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return QString();
        if (writer_.joinable()) writer_.join();
        drain();                       /* whatever arrived after the last pass */
        const uint64_t n = frames_.load(std::memory_order_relaxed);
        out_.flush();
        writeHeader(n);                /* now that the length is known */
        out_.close();
        return path_;
    }

    bool     active()  const { return running_.load(std::memory_order_acquire); }
    uint64_t frames()  const { return frames_.load(std::memory_order_relaxed); }
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

    /* Audio thread only. */
    void feed(const float *interleaved, int frames)
    {
        if (!running_.load(std::memory_order_relaxed)) return;
        const size_t cap = ring_.size();
        const uint64_t h = head_.load(std::memory_order_relaxed);
        const uint64_t t = tailShadow_.load(std::memory_order_acquire);
        const size_t want = size_t(frames) * 2;
        if (h - t + want > cap) {                  /* writer has fallen behind */
            dropped_.fetch_add(uint64_t(frames), std::memory_order_relaxed);
            return;
        }
        for (size_t i = 0; i < want; i++)
            ring_[(h + i) % cap] = interleaved[i];
        head_.store(h + want, std::memory_order_release);
    }

private:
    static const int kRingSeconds = 4;

    void drainLoop()
    {
        while (running_.load(std::memory_order_acquire)) {
            drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    void drain()
    {
        bool wrote = false;
        const size_t cap = ring_.size();
        const uint64_t h = head_.load(std::memory_order_acquire);
        static thread_local std::vector<int16_t> pcm;
        while (tail_ < h) {
            const size_t chunk = size_t(std::min<uint64_t>(h - tail_, 8192));
            pcm.resize(chunk);
            for (size_t i = 0; i < chunk; i++) {
                float v = ring_[(tail_ + i) % cap];
                v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
                pcm[i] = int16_t(v * 32767.0f);
            }
            out_.write(reinterpret_cast<const char *>(pcm.data()),
                       std::streamsize(chunk * sizeof(int16_t)));
            tail_ += chunk;
            frames_.fetch_add(chunk / 2, std::memory_order_relaxed);
            wrote = true;
        }
        tailShadow_.store(tail_, std::memory_order_release);
        /* Keep the header honest as the take grows. The length is only known at
         * the end, so the obvious thing is to patch it there -- but then a
         * session that is killed or crashes leaves a file whose header says
         * zero frames while the data is all present, and nothing will play it.
         * Verified: an interrupted take held 2.09 s of audio and reported 0.00.
         * Rewriting it each pass costs one seek per 20 ms and makes the file
         * playable at any moment. */
        if (wrote) writeHeader(frames_.load(std::memory_order_relaxed));
    }

    void writeHeader(uint64_t frames)
    {
        const uint32_t bytes = uint32_t(frames * 2 * sizeof(int16_t));
        unsigned char h[44];
        memcpy(h, "RIFF", 4);
        { uint32_t v = 36 + bytes;      memcpy(h + 4, &v, 4); }
        memcpy(h + 8, "WAVEfmt ", 8);
        { uint32_t v = 16;              memcpy(h + 16, &v, 4); }
        { uint16_t v = 1;               memcpy(h + 20, &v, 2); }
        { uint16_t v = 2;               memcpy(h + 22, &v, 2); }
        { uint32_t v = uint32_t(rate_); memcpy(h + 24, &v, 4); }
        { uint32_t v = uint32_t(rate_) * 4; memcpy(h + 28, &v, 4); }
        { uint16_t v = 4;               memcpy(h + 32, &v, 2); }
        { uint16_t v = 16;              memcpy(h + 34, &v, 2); }
        memcpy(h + 36, "data", 4);
        memcpy(h + 40, &bytes, 4);
        const auto keep = out_.tellp();
        out_.seekp(0);
        out_.write(reinterpret_cast<const char *>(h), sizeof h);
        /* Back to where the data was being appended. Seeking to 0 and not
         * returning would overwrite the take with itself from the start. */
        if (keep > std::streampos(0)) out_.seekp(keep);
        out_.flush();
    }

    std::vector<float>    ring_;
    std::atomic<uint64_t> head_{0};
    std::atomic<uint64_t> tailShadow_{0};
    uint64_t              tail_ = 0;      /* writer thread only */
    std::atomic<uint64_t> frames_{0}, dropped_{0};
    std::atomic<bool>     running_{false};
    std::thread           writer_;
    std::ofstream         out_;
    QString               path_;
    int                   rate_ = 48000;
};

/* ----------------------------------------------------------------- engine */

/* Owns the plugin and the PipeWire stream. The park handshake exists because
 * loading a new plugin frees the old one while the realtime callback may be
 * inside pehost_render(). */
class Engine {
public:
    ~Engine() { stopAudio(); unload(); }

    bool startAudio(QString *err)
    {
        pw_init(nullptr, nullptr);
        buf_ = static_cast<float *>(calloc(size_t(kMaxFrames) * 2, sizeof(float)));
        in_  = static_cast<float *>(calloc(size_t(kMaxFrames) * 2, sizeof(float)));
        if (!buf_ || !in_) { *err = "out of memory"; return false; }

        loop_ = pw_thread_loop_new("pestudio", nullptr);
        if (!loop_) { *err = "pw_thread_loop_new failed"; return false; }

        char lat[64];
        snprintf(lat, sizeof lat, "%d/%d", kQuantum, kSampleRate);
        static const pw_stream_events ev = {
            .version = PW_VERSION_STREAM_EVENTS,
            .process = &Engine::onProcess,
        };
        stream_ = pw_stream_new_simple(
            pw_thread_loop_get_loop(loop_), "pestudio",
            pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                              PW_KEY_MEDIA_CATEGORY, "Playback",
                              PW_KEY_MEDIA_ROLE, "Music",
                              PW_KEY_NODE_LATENCY, lat, nullptr),
            &ev, this);
        if (!stream_) { *err = "pw_stream_new_simple failed"; return false; }

        uint8_t pod[1024];
        spa_pod_builder bb = SPA_POD_BUILDER_INIT(pod, sizeof pod);
        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32;
        info.rate = kSampleRate;
        info.channels = 2;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
        const spa_pod *params[1] = { spa_format_audio_raw_build(&bb, SPA_PARAM_EnumFormat, &info) };

        if (pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                              pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT |
                                              PW_STREAM_FLAG_MAP_BUFFERS |
                                              PW_STREAM_FLAG_RT_PROCESS),
                              params, 1) < 0) {
            *err = "pw_stream_connect failed"; return false;
        }
        if (pw_thread_loop_start(loop_) < 0) { *err = "pw_thread_loop_start failed"; return false; }
        running_ = true;
        return true;
    }

    void stopAudio()
    {
        if (loop_)   pw_thread_loop_stop(loop_);
        if (stream_) { pw_stream_destroy(stream_); stream_ = nullptr; }
        if (loop_)   { pw_thread_loop_destroy(loop_); loop_ = nullptr; }
        free(buf_); buf_ = nullptr;
        free(in_);  in_  = nullptr;
        running_ = false;
    }

    /* Stop the realtime callback touching the plugin, then wait for it to say
     * so. Bounded, because a suspended node never calls back and an unbounded
     * spin would freeze the UI.
     *
     * Running out of patience is not on its own a reason to carry on. A node
     * that never calls back and one whose render is merely slow look identical
     * from here, and only the second makes freeing the plugin fatal -- so the
     * callback counter is sampled to tell them apart. Every callback either
     * parks or bumps that counter, so a counter that has not moved means nothing
     * was running and there is nothing to pull the plugin out from under.
     * False means the opposite, and the caller must not free. */
    bool park()
    {
        if (!running_) return true;
        const unsigned long before = calls_.load(std::memory_order_relaxed);
        parked_.store(false, std::memory_order_relaxed);
        parkReq_.store(true, std::memory_order_release);
        for (int i = 0; i < 1000; i++) {
            if (parked_.load(std::memory_order_acquire)) return true;
            QThread::usleep(500);
        }
        if (calls_.load(std::memory_order_relaxed) != before) {
            qWarning("park timed out with the audio callback still rendering");
            return false;
        }
        return true;      /* never called back at all -- nothing to park */
    }
    void unpark() { parkReq_.store(false, std::memory_order_release); }

    bool load(const QString &path, QString *err, pehost_kind kind = PEHOST_KIND_AUTO)
    {
        if (!park()) {
            unpark();
            *err = "the audio callback would not stop; refusing to unload a "
                   "plugin that is still rendering";
            return false;
        }
        {
            std::lock_guard<std::mutex> g(hostMx_);
            unloadLocked();
            host_ = pehost_open_as(path.toLocal8Bit().constData(), kind, kSampleRate, kQuantum);
        }
        unpark();
        if (!host_) { *err = QString::fromUtf8(pehost_last_error()); return false; }
        return true;
    }
    void unload()
    {
        std::lock_guard<std::mutex> g(hostMx_);
        unloadLocked();
    }

    /* Every use of the plugin from off the audio thread goes through here.
     *
     * park() stops the *audio callback* and says nothing about anyone else, and
     * MIDI now arrives on its own reader thread. A note landing while the GUI
     * swapped plugins therefore used a pointer pehost_close had already freed --
     * which is exactly a segfault while playing. Testing host() and then calling
     * it was racy for the same reason: it could be freed between the two. */
    template <class F> void withHost(F fn)
    {
        std::lock_guard<std::mutex> g(hostMx_);
        if (host_) fn(host_);
    }

    pehost *host() const { return host_; }

private:
    void unloadLocked()
    {
        if (host_) { pehost_close(host_); host_ = nullptr; }
    }
    /* Held by anything that loads, closes, or uses the plugin off the audio
     * thread. Never taken by the audio callback -- that is what park() is for. */
    std::mutex hostMx_;
public:
    bool    audioRunning() const { return running_; }
    unsigned long callbacks() const { return calls_.load(std::memory_order_relaxed); }
    float   peak() { return peak_.exchange(0.0f, std::memory_order_relaxed); }

private:
    static const int kMaxFrames = 8192;

    static void onProcess(void *ud) { static_cast<Engine *>(ud)->process(); }

    void process()
    {
        if (!tebReady_) { pehost_thread_init(); tebReady_ = true; }

        pw_buffer *b = pw_stream_dequeue_buffer(stream_);
        if (!b) return;
        spa_buffer *sb = b->buffer;
        float *dst = static_cast<float *>(sb->datas[0].data);
        if (!dst) { pw_stream_queue_buffer(stream_, b); return; }

        int n = int(sb->datas[0].maxsize / (sizeof(float) * 2));
        if (b->requested && int(b->requested) < n) n = int(b->requested);
        if (n > kMaxFrames) n = kMaxFrames;

        if (parkReq_.load(std::memory_order_acquire)) {
            parked_.store(true, std::memory_order_release);
            memset(dst, 0, size_t(n) * 2 * sizeof(float));
            /* Recorded as silence rather than skipped: a plugin swap mid-take
             * should leave a gap you can hear, not shorten the recording and
             * pull everything after it earlier. */
            rec_.feed(dst, n);
        } else {
            parked_.store(false, std::memory_order_release);
            /* A synth makes its own sound and wants nothing fed to it; an effect
             * needs something to work on or it can only output silence. */
            /* `n` can exceed kQuantum: PipeWire's requested latency is only a
             * request and the graph quantum may be larger. pehost_render_io
             * splits the block so the plugin never sees more frames than it was
             * promised, so there is nothing to do about it here. */
            if (host_ && !pehost_is_synth(host_) && source() != SrcSilence) {
                fillInput(n);
                pehost_render_io(host_, in_, buf_, n);
            } else {
                pehost_render(host_, buf_, n);
            }
            float pk = 0.0f;
            for (int i = 0; i < n * 2; i++) {
                float v = buf_[i] * gain_;
                if (v >  1.0f) v =  1.0f;
                if (v < -1.0f) v = -1.0f;
                float a = v < 0 ? -v : v;
                if (a > pk) pk = a;
                dst[i] = v;
            }
            float cur = peak_.load(std::memory_order_relaxed);
            if (pk > cur) peak_.store(pk, std::memory_order_relaxed);
            calls_.fetch_add(1, std::memory_order_relaxed);
            /* After the gain and the clip, so the file is what came out of the
             * speakers rather than what the plugin produced before the fader. */
            rec_.feed(dst, n);
        }

        sb->datas[0].chunk->offset = 0;
        sb->datas[0].chunk->stride = sizeof(float) * 2;
        sb->datas[0].chunk->size   = uint32_t(n * 2 * sizeof(float));
        pw_stream_queue_buffer(stream_, b);
    }

public:
    Recorder rec_;
    float gain_ = 0.8f;

    /* What to feed a plugin that processes rather than generates. An effect given
     * silence correctly produces silence, so with no source at all a compressor
     * or a gate looks broken when it is working perfectly. */
    enum Source { SrcSilence = 0, SrcNotes = 1, SrcNoise = 2 };
    void setSource(Source s) { src_.store(int(s), std::memory_order_relaxed); }
    Source source() const { return Source(src_.load(std::memory_order_relaxed)); }

    /* Called from the GUI thread; the audio thread owns the voices themselves and
     * only reads these gates, so no lock is needed either way. */
    void noteOn(int n, int vel)
    { if (n >= 0 && n < 128) gate_[n].store(uint8_t(vel ? vel : 100), std::memory_order_relaxed); }
    void noteOff(int n)
    { if (n >= 0 && n < 128) gate_[n].store(0, std::memory_order_relaxed); }
    void allNotesOff()
    { for (int i = 0; i < 128; i++) gate_[i].store(0, std::memory_order_relaxed); }

private:
    pehost              *host_   = nullptr;
    pw_thread_loop      *loop_   = nullptr;
    pw_stream           *stream_ = nullptr;
    float               *buf_    = nullptr;
    bool                 running_ = false;
    bool                 tebReady_ = false;      /* audio thread only */
    std::atomic<bool>    parkReq_{false}, parked_{false};
    std::atomic<unsigned long> calls_{0};
    std::atomic<float>   peak_{0.0f};

    /* The input signal, for effects. */
    float               *in_ = nullptr;
    std::atomic<int>     src_{SrcSilence};
    std::atomic<uint8_t> gate_[128] = {};
    float                env_[128] = {};        /* audio thread only */
    double               phase_[128] = {};
    uint32_t             rng_ = 0x12345678u;

    /* One block of input. A sawtooth per held note with a short attack and a
     * gentle release: an effect wants harmonics and an envelope to work on, and a
     * pure sine tells you very little about a compressor. */
    void fillInput(int n)
    {
        const double sr = double(kSampleRate);
        int i, note;

        memset(in_, 0, size_t(n) * 2 * sizeof(float));
        if (source() == SrcNoise) {
            for (i = 0; i < n; i++) {
                rng_ = rng_ * 1664525u + 1013904223u;
                float v = float(int32_t(rng_) >> 8) / 8388608.0f * 0.25f;
                in_[2 * i] = in_[2 * i + 1] = v;
            }
            return;
        }
        if (source() != SrcNotes) return;

        for (note = 0; note < 128; note++) {
            float target = float(gate_[note].load(std::memory_order_relaxed)) / 127.0f;
            if (target <= 0.0f && env_[note] <= 1e-5f) { env_[note] = 0.0f; continue; }
            {
                /* 5 ms attack, 120 ms release, per sample. */
                const float up = 1.0f - expf(-1.0f / (0.005f * float(sr)));
                const float dn = 1.0f - expf(-1.0f / (0.120f * float(sr)));
                double step = 440.0 * pow(2.0, (note - 69) / 12.0) / sr;
                for (i = 0; i < n; i++) {
                    float e = env_[note];
                    e += ((target > 0.0f ? target : 0.0f) - e) * (target > e ? up : dn);
                    env_[note] = e;
                    phase_[note] += step;
                    if (phase_[note] >= 1.0) phase_[note] -= 1.0;
                    {
                        /* A saw, scaled so a fistful of keys does not clip the
                         * plugin's input before it has had a chance to act. */
                        float v = float(2.0 * phase_[note] - 1.0) * e * 0.18f;
                        in_[2 * i]     += v;
                        in_[2 * i + 1] += v;
                    }
                }
            }
        }
    }
};

/* ------------------------------------------------------------- re-entrancy */

/* A Classic editor tracks a drag by spinning on the mouse, so the host has to let
 * events through from inside that spin -- see cfm_set_input_pump. That makes every
 * call into a plugin potentially re-entrant: pumping Qt can deliver a click on the
 * plugin list, or fire the editor's own timer, and either would call back into a
 * plugin that is already running. Unloading one mid-call frees the interpreter
 * underneath itself; calling in again deadlocks on the lock that serialises guest
 * execution. So anything that enters a plugin raises this, and anything that might
 * be re-entered checks it. */
static int g_inPlugin;

struct PluginCall {
    PluginCall()  { ++g_inPlugin; }
    ~PluginCall() { --g_inPlugin; }
};

/* ---------------------------------------------------------------- keyboard */

class Piano : public QWidget {
    Q_OBJECT
public:
    explicit Piano(QWidget *p = nullptr) : QWidget(p)
    {
        setFixedHeight(int(kKeyW * 4.5));
        setFocusPolicy(Qt::StrongFocus);
        held_.fill(false);
    }
    void setHeld(int note, bool on)
    {
        if (note >= kLow && note < kLow + kKeys) { held_[note - kLow] = on; update(); }
    }
signals:
    void noteOn(int note, int vel);
    void noteOff(int note);

protected:
    static const int kLow  = 36;      /* C2 */
    /* How many keys can exist, not how many are drawn: the width decides that,
     * and this is the ceiling it works up to -- C2 to C8, which is more than a
     * real keyboard and more than any window will ask for. */
    static const int kKeys = 92;

    static bool isBlack(int n) { int s = n % 12; return s==1||s==3||s==6||s==8||s==10; }

    /* "C4" for middle C, matching the octave numbering the C labels already
     * used. The accidental is a real sharp sign rather than a hash: at the size
     * a black key allows, "#" reads as a smudge. */
    static QString noteName(int n, bool withOctave = true)
    {
        static const char *nm[12] = { "C", "C♯", "D", "D♯", "E", "F",
                                      "F♯", "G", "G♯", "A", "A♯", "B" };
        return withOctave ? QString("%1%2").arg(nm[n % 12]).arg(n / 12 - 1)
                          : QString(nm[n % 12]);
    }

    /* A key keeps its size; a wider window shows more of the keyboard rather
     * than the same keys stretched flatter. Fixed at four octaves across the
     * whole width, a 1900 px window drew keys nearly an inch across and a third
     * of an inch tall -- a strip, not a keyboard. An octave is a hand span
     * whatever the room is like. */
    static constexpr double kKeyW = 24.0;    /* white key width, fixed */
    static const int kTop = kLow + kKeys - 1;   /* 127, the top of MIDI */

    /* Where the leftmost key starts: centred once every note MIDI has is on
     * screen and there is width to spare, hard left otherwise. */
    int keysX0() const
    {
        const double used = whiteCount() * kKeyW;
        return used < width() ? int((width() - used) / 2.0) : 0;
    }

    /* How many white keys fit, and the highest note that reaches. */
    int visibleWhites() const
    { return qMax(1, int(double(width()) / kKeyW)); }

    int highestNote() const
    {
        int seen = 0, hi = kLow;
        for (int n = kLow; n <= kTop; ++n) {
            if (isBlack(n)) continue;
            hi = n;
            if (++seen >= visibleWhites()) break;
        }
        return hi;
    }

    int whiteCount() const
    {
        int c = 0;
        for (int n = kLow; n <= highestNote(); ++n) if (!isBlack(n)) c++;
        return c;
    }

    QRect whiteRect(int idx) const
    { return QRect(keysX0() + int(idx * kKeyW), 0, int(kKeyW) + 1, height()); }

    void paintEvent(QPaintEvent *) override
    {
        QPainter g(this);
        g.fillRect(rect(), QColor(24, 24, 28));
        const int hi = highestNote();
        int wi = 0;
        // white keys first
        for (int n = kLow; n <= hi; n++) {
            const int i = n - kLow;
            if (isBlack(n)) continue;
            QRect r = whiteRect(wi++);
            g.setBrush(held_[i] ? QColor(120, 170, 255) : QColor(238, 238, 240));
            g.setPen(QColor(60, 60, 66));
            g.drawRect(r.adjusted(0, 0, -1, -1));
            /* Every key named, not just the Cs. The octave is dropped when the
             * key is too narrow to hold it -- a truncated "C" is still the note,
             * a truncated "C4" is a lie about which one. */
            QFont f = g.font();
            f.setPointSizeF(qBound(6.0, r.width() * 0.30, 10.0));
            g.setFont(f);
            const bool room = QFontMetrics(f).horizontalAdvance(noteName(n)) <= r.width() - 4;
            g.setPen(n % 12 == 0 ? QColor(70, 70, 80) : QColor(130, 130, 140));
            g.drawText(r.adjusted(1, 0, -1, -3), Qt::AlignBottom | Qt::AlignHCenter,
                       noteName(n, room));
        }
        // black keys on top
        wi = 0;
        for (int n = kLow; n <= hi; n++) {
            const int i = n - kLow;
            if (isBlack(n)) continue;
            QRect r = whiteRect(wi++);
            if (n + 1 <= hi && isBlack(n + 1)) {
                QRect b(r.right() - r.width() / 4, 0, r.width() / 2, height() * 3 / 5);
                g.setBrush(held_[i + 1] ? QColor(70, 120, 210) : QColor(20, 20, 24));
                g.setPen(QColor(0, 0, 0));
                g.drawRect(b);
                /* Black keys carry the accidental only. At half a white key
                 * wide there is no room for the octave, and the neighbouring
                 * white key already says which one it is. */
                QFont f = g.font();
                f.setPointSizeF(qBound(5.5, b.width() * 0.46, 9.0));
                g.setFont(f);
                g.setPen(QColor(190, 190, 200));
                g.drawText(b.adjusted(0, 0, 0, -3), Qt::AlignBottom | Qt::AlignHCenter,
                           noteName(kLow + i + 1, false));
            }
        }
    }

    int noteAt(const QPoint &p) const
    {
        const int hi = highestNote();
        int wi = 0;
        // black keys take precedence: they sit on top
        for (int n = kLow; n <= hi; n++) {
            if (isBlack(n)) continue;
            QRect r = whiteRect(wi++);
            if (n + 1 <= hi && isBlack(n + 1)) {
                QRect b(r.right() - r.width() / 4, 0, r.width() / 2, height() * 3 / 5);
                if (b.contains(p)) return n + 1;
            }
        }
        wi = 0;
        for (int n = kLow; n <= hi; n++) {
            if (isBlack(n)) continue;
            if (whiteRect(wi++).contains(p)) return n;
        }
        return -1;
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        int n = noteAt(e->pos());
        if (n >= 0) { last_ = n; setHeld(n, true); emit noteOn(n, 100); }
    }
    void mouseReleaseEvent(QMouseEvent *) override
    {
        if (last_ >= 0) { setHeld(last_, false); emit noteOff(last_); last_ = -1; }
    }

public:
    /* Release everything still held.
     *
     * Held notes are tracked rather than inferred, because the release event that
     * would have ended one does not always arrive: clicking the editor to adjust a
     * control moves focus away from this widget, and the key-up then goes to the
     * editor instead. The note-off was never sent and the note stuck -- which is
     * the whole of "the keys stick sometimes". */
    void releaseAll()
    {
        int n, released = 0;
        for (n = 0; n < kKeys; n++) {
            if (!held_[n]) continue;
            held_[n] = false;
            emit noteOff(kLow + n);
            released++;
        }
        last_ = -1;
        if (released) {
            fprintf(stderr, "piano: released %d held note(s)\n", released);
            update();
        }
    }

    /* Route a note key here whatever has focus. Returns true if it was a note.
     *
     * Held notes cannot be tracked by this widget's own focus, because the point
     * of holding one is to go and turn a knob -- which moves focus to the editor,
     * and the key-up with it. Releasing on focus loss stopped notes sticking and
     * made it impossible to hear an edit on a sounding note, which is the whole
     * reason to hold one. So the release is caught application-wide instead. */
    bool routeKey(int qtKey, bool down, bool autoRepeat)
    {
        int n = keyNote(qtKey);
        if (n < 0) return false;
        if (autoRepeat) return true;
        if (down) { if (!isHeld(n)) { setHeld(n, true); emit noteOn(n, 100); } }
        else      { if (isHeld(n))  { setHeld(n, false); emit noteOff(n); } }
        return true;
    }

    bool isHeld(int note) const
    { return note >= kLow && note < kLow + kKeys && held_[note - kLow]; }

protected:
    void hideEvent(QHideEvent *e) override
    { QWidget::hideEvent(e); releaseAll(); }

    void keyPressEvent(QKeyEvent *e) override
    {
        if (!routeKey(e->key(), true, e->isAutoRepeat())) QWidget::keyPressEvent(e);
    }
    void keyReleaseEvent(QKeyEvent *e) override
    {
        if (!routeKey(e->key(), false, e->isAutoRepeat())) QWidget::keyReleaseEvent(e);
    }

    /* Two rows, tracker style: zsxdcvgbhnjm = lower octave, q2w3er5t6y7u = upper */
    static int keyNote(int k)
    {
        static const char lo[] = "zsxdcvgbhnjm";
        static const char hi[] = "q2w3er5t6y7u";
        if (k < 0 || k > 0x10FFFF) return -1;
        char c = char(QChar(k).toLower().toLatin1());
        if (const char *p = strchr(lo, c)) if (c) return 48 + int(p - lo);
        if (const char *p = strchr(hi, c)) if (c) return 60 + int(p - hi);
        return -1;
    }

private:
    std::array<bool, kKeys> held_{};
    int last_ = -1;
};

/* ------------------------------------------------------------- pitch wheel */

/* The sprung wheel a synth keyboard puts to the left of its keys.
 *
 * Sprung is the whole of it. A bend left off centre detunes everything played
 * afterwards, and nothing downstream can tell that the user stopped meaning it,
 * so the wheel returns to centre the moment it is let go and sends that centre --
 * exactly what the hardware does. Panic recentres it for the same reason it
 * releases held notes: a bend stuck at the top is as wrong as a note stuck on,
 * and harder to recognise as the cause, because the plugin goes on sounding
 * correct and merely in the wrong key.
 *
 * The value stays in MIDI's 14-bit form (0..16383, 8192 at rest) rather than
 * being converted to semitones, because how far the wheel reaches is the
 * plugin's business: bend range is a plugin parameter, and converting here would
 * mean guessing it.
 *
 * Dragging is relative to where the wheel was grabbed rather than absolute to
 * the cursor. An absolute mapping snaps to full bend when the wheel is grabbed
 * near an end, and the way to a small bend should not be a large one. */
class PitchWheel : public QWidget {
    Q_OBJECT
public:
    static constexpr int kCentre = 8192;
    static constexpr int kMax    = 16383;

    explicit PitchWheel(QWidget *p = nullptr) : QWidget(p)
    {
        setFixedWidth(40);
        setMinimumHeight(84);
        /* Letters belong to the piano wherever the pointer is. Taking focus here
         * would only mean the note keys stopped playing after a bend. */
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::SizeVerCursor);
        setToolTip("Pitch wheel — drag up or down; springs back to centre");
    }

    int value() const { return v_; }

    /* What an external wheel is doing, shown but not re-sent: echoing it back
     * would put it straight out of the port it just arrived from. */
    void setBend(int v14)
    {
        v14 = qBound(0, v14, int(kMax));
        if (v14 == v_) return;
        v_ = v14;
        update();
    }

    /* Back to centre, and say so. Deliberately sends even when already centred:
     * one redundant message is the cheap way to be sure the plugin agrees. */
    void recentre()
    {
        dragging_ = false;
        v_ = kCentre;
        update();
        emit bend(v_);
    }

signals:
    void bend(int value14);

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) { QWidget::mousePressEvent(e); return; }
        dragging_ = true;
        grabY_ = e->position().y();
        grabV_ = v_;
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (!dragging_) return;
        /* Half the height reaches full bend in either direction, so the travel
         * on screen is the travel of the thing being imitated. */
        const double travel = qMax(8.0, height() / 2.0 - 6.0);
        const double dy = grabY_ - e->position().y();          /* up is sharp */
        const int nv = qBound(0, grabV_ + int(dy / travel * kCentre), int(kMax));
        if (nv == v_) return;      /* also keeps the port off a repeat message */
        v_ = nv;
        update();
        emit bend(v_);
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) { QWidget::mouseReleaseEvent(e); return; }
        recentre();
    }
    /* Hidden mid-drag there is no release to come, and the bend would be held
     * for good -- the same trap Piano::hideEvent covers for notes. */
    void hideEvent(QHideEvent *e) override { QWidget::hideEvent(e); recentre(); }

    void paintEvent(QPaintEvent *) override
    {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing, true);
        g.fillRect(rect(), QColor(24, 24, 28));

        const int label = 12;
        const QRectF body(7.0, 4.0, width() - 14.0, height() - 4.0 - label);
        if (body.height() < 8.0) return;

        const double off = double(v_ - kCentre) / kCentre;     /* -1 .. +1 */

        /* A cylinder seen edge on: dark at the rims, lit across the middle. */
        QLinearGradient lg(body.left(), 0, body.right(), 0);
        lg.setColorAt(0.00, QColor(18, 18, 22));
        lg.setColorAt(0.35, QColor(74, 74, 84));
        lg.setColorAt(0.50, QColor(98, 98, 110));
        lg.setColorAt(0.65, QColor(74, 74, 84));
        lg.setColorAt(1.00, QColor(18, 18, 22));
        g.setPen(QPen(QColor(60, 60, 66), 1));
        g.setBrush(lg);
        g.drawRoundedRect(body, 5, 5);

        g.save();
        QPainterPath clip;
        clip.addRoundedRect(body, 5, 5);
        g.setClipPath(clip);

        /* Ridges roll with the value. That is what makes the travel legible at a
         * glance -- a bare marker line on a strip reads as a slider.
         *
         * Two and a half ridges of roll, not three: a whole number of them puts
         * full deflection back in phase with centre, and the wheel then looks
         * untouched at exactly the position where it is furthest from rest. */
        const double spacing = 7.0;
        const double roll = -off * spacing * 2.5;
        g.setPen(QPen(QColor(0, 0, 0, 90), 1));
        for (double y = std::fmod(roll, spacing) - spacing;
             y < body.height() + spacing; y += spacing) {
            const double yy = body.top() + y;
            if (yy < body.top() || yy > body.bottom()) continue;
            g.drawLine(QPointF(body.left(), yy), QPointF(body.right(), yy));
        }

        /* The grip, in the colour a held key uses once it is off centre. */
        const double my = body.center().y() - off * (body.height() / 2.0 - 4.0);
        g.setPen(QPen(v_ == kCentre ? QColor(150, 150, 160) : QColor(120, 170, 255), 2));
        g.drawLine(QPointF(body.left() + 1, my), QPointF(body.right() - 1, my));
        g.restore();

        /* Detent marks on the frame: where centre is, whatever the wheel says. */
        const double cy = body.center().y();
        g.setPen(QColor(90, 90, 100));
        g.drawLine(QPointF(1, cy), QPointF(5, cy));
        g.drawLine(QPointF(width() - 5, cy), QPointF(width() - 1, cy));

        QFont f = g.font();
        f.setPointSizeF(6.5);
        g.setFont(f);
        g.setPen(QColor(130, 130, 140));
        g.drawText(QRect(0, height() - label, width(), label),
                   Qt::AlignHCenter | Qt::AlignVCenter, "PITCH");
    }

private:
    int    v_ = kCentre;
    bool   dragging_ = false;
    double grabY_ = 0.0;
    int    grabV_ = kCentre;
};

/* ------------------------------------------------------- parameter table */

/* A model rather than a widget per parameter. VST3 plugins routinely expose
 * thousands of parameters -- Surge XT has 2855 -- and building a slider and two
 * labels for each took long enough to look like a hang. A QTableView only ever
 * realises the rows on screen, so load time stops depending on the count, and
 * only visible rows are queried from the plugin. */
class ParamModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum { ColName, ColValue, ColDisplay, ColCount };

    void setHost(pehost *h)
    {
        beginResetModel();
        host_ = h;
        rows_ = h ? pehost_num_params(h) : 0;
        endResetModel();
    }

    int rowCount(const QModelIndex &p = QModelIndex()) const override
    { return p.isValid() ? 0 : rows_; }
    int columnCount(const QModelIndex &p = QModelIndex()) const override
    { return p.isValid() ? 0 : ColCount; }

    QVariant headerData(int s, Qt::Orientation o, int role) const override
    {
        if (role != Qt::DisplayRole) return QVariant();
        if (o == Qt::Vertical) return s;
        switch (s) {
        case ColName:    return "Parameter";
        case ColValue:   return "Value";
        case ColDisplay: return "";
        }
        return QVariant();
    }

    QVariant data(const QModelIndex &ix, int role) const override
    {
        if (!host_ || !ix.isValid() || ix.row() >= rows_) return QVariant();
        const int r = ix.row();
        if (role == Qt::DisplayRole) {
            if (ix.column() == ColName) {
                char nm[64];
                pehost_param_name(host_, r, nm, sizeof nm);
                QString t = QString::fromLocal8Bit(nm).trimmed();
                return t.isEmpty() ? QString("Param %1").arg(r) : t;
            }
            if (ix.column() == ColDisplay) {
                char ds[64], lb[64];
                pehost_param_display(host_, r, ds, sizeof ds);
                pehost_param_label(host_, r, lb, sizeof lb);
                QString t = QString::fromLocal8Bit(ds).trimmed();
                QString u = QString::fromLocal8Bit(lb).trimmed();
                if (!u.isEmpty()) t += " " + u;
                return t;
            }
            return QVariant();
        }
        if (role == Qt::UserRole && ix.column() == ColValue)
            return double(pehost_get_param(host_, r));
        if (role == Qt::TextAlignmentRole && ix.column() == ColDisplay)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return QVariant();
    }

    bool setData(const QModelIndex &ix, const QVariant &v, int role) override
    {
        if (!host_ || !ix.isValid() || role != Qt::UserRole) return false;
        pehost_set_param(host_, ix.row(), float(v.toDouble()));
        emit dataChanged(index(ix.row(), ColValue), index(ix.row(), ColDisplay));
        return true;
    }

    /* Repaint a span without touching the plugin for rows nobody can see. */
    void refresh(int first, int last)
    {
        if (rows_ <= 0) return;
        first = qBound(0, first, rows_ - 1);
        last  = qBound(0, last,  rows_ - 1);
        if (last < first) return;
        emit dataChanged(index(first, ColValue), index(last, ColDisplay));
    }

private:
    pehost *host_ = nullptr;
    int     rows_ = 0;
};

/* Draws the value column as a bar. */
class BarDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &ix) const override
    {
        if (ix.column() != ParamModel::ColValue) { QStyledItemDelegate::paint(p, o, ix); return; }
        const double v = qBound(0.0, ix.data(Qt::UserRole).toDouble(), 1.0);
        QRect r = o.rect.adjusted(3, 4, -3, -4);
        p->save();
        p->setPen(o.palette.mid().color());
        p->setBrush(Qt::NoBrush);
        p->drawRect(r);
        if (v > 0.0) {
            QRect f = r.adjusted(1, 1, -1, -1);
            f.setWidth(qMax(1, int(f.width() * v)));
            p->fillRect(f, o.palette.highlight());
        }
        p->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem &o, const QModelIndex &ix) const override
    { QSize s = QStyledItemDelegate::sizeHint(o, ix); s.setHeight(qMax(s.height(), 22)); return s; }
};

/* Click or drag anywhere in the value column to set it. Handled in the view
 * rather than the delegate because view-level mouse handling is predictable
 * without fighting the edit-trigger machinery. */
class ParamTable : public QTableView {
    Q_OBJECT
public:
    explicit ParamTable(QWidget *p = nullptr) : QTableView(p) {}

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && apply(e->position().toPoint())) { dragging_ = true; return; }
        QTableView::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (dragging_ && (e->buttons() & Qt::LeftButton)) { apply(e->position().toPoint()); return; }
        QTableView::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (dragging_) { dragging_ = false; return; }
        QTableView::mouseReleaseEvent(e);
    }

private:
    bool apply(const QPoint &pos)
    {
        const QModelIndex ix = indexAt(pos);
        if (!ix.isValid() || ix.column() != ParamModel::ColValue || !model()) return false;
        const QRect r = visualRect(ix).adjusted(4, 0, -4, 0);
        double v = r.width() > 0 ? double(pos.x() - r.left()) / r.width() : 0.0;
        model()->setData(ix, qBound(0.0, v, 1.0), Qt::UserRole);
        return true;
    }
    bool dragging_ = false;
};

/* ----------------------------------------------------------- plugin editor */

/* Hosts the plugin's own GUI. VST3 editors draw themselves into a native window
 * the host supplies; on Linux that is an X11 window id, so this widget just
 * hands over its winId() and gets out of the way -- there is no drawing code
 * here, and what appears is the plugin's real interface.
 *
 * Linux VST3 plugins also expect the host to own the event loop: they register
 * X11 descriptors and timers with us instead of running their own, and a
 * JUCE-based editor will sit blank without it. QSocketNotifier and QTimer map
 * onto those registrations directly. */
class EditorHost : public QWidget {
    Q_OBJECT
public:
    explicit EditorHost(QWidget *p = nullptr) : QWidget(p)
    {
        /* A native window is required: the plugin needs a real X11 id, not an
         * id borrowed from an ancestor. */
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_DontCreateNativeAncestors);
        setMinimumSize(320, 200);
        install();
    }
    /* detach() first, so unregisters the plugin makes on the way out are still
     * routed; then the hooks go, because they carry a pointer to this object and
     * a plugin left able to register against a destroyed host is the same
     * dangling-callback fault the watches themselves had. */
    ~EditorHost() override { detach(); v3_set_runloop_hooks(nullptr); self_ = nullptr; }

    bool attach(pehost *h)
    {
        detach();
        if (!h || !pehost_has_editor(h)) return false;
        int w = 0, ht = 0;
        pehost_editor_size(h, &w, &ht);
        if (w > 0 && ht > 0) { natW_ = w; natH_ = ht; setMinimumSize(1, 1); resize(w, ht); }
        /* Realise the window before handing its id over. */
        (void)winId();
        /* winId() is only an X11 Window on the xcb backend. On Wayland it is a
         * surface handle, and handing that to a plugin as an X11 embed id makes
         * the plugin run Xlib against a window that does not exist -- it dies
         * inside its own toolkit, which reads as a crash on load. Refuse with a
         * reason instead. main() asks for xcb up front, so this is the backstop
         * for someone overriding QT_QPA_PLATFORM. */
        const QString platform = QGuiApplication::platformName();
        if (!platform.startsWith(QLatin1String("xcb"))) {
            fprintf(stderr, "editor: the %s platform cannot host an X11 plugin "
                            "editor -- run with QT_QPA_PLATFORM=xcb\n",
                    qPrintable(platform));
            return false;
        }
        if (pehost_editor_attach(h, (unsigned long)winId()) != 0) {
            fprintf(stderr, "editor: the plugin refused to embed into window "
                            "0x%lx\n", (unsigned long)winId());
            return false;
        }
        fprintf(stderr, "editor: embedded as a child of window 0x%lx (%dx%d)\n",
                (unsigned long)winId(), w, ht);
        host_ = h;
        return true;
    }
    void detach()
    {
        if (host_) { pehost_editor_detach(host_); host_ = nullptr; }
        natW_ = natH_ = 0;
        clearWatches();
    }
    bool attached() const { return host_ != nullptr; }

    /* There is no zooming a native editor.
     *
     * The plug-in owns an X11 window of its own and paints it itself; nothing
     * on this side is in the path, so there is no image to scale -- X has no
     * notion of a scaled child window either. What can be done is to hand the
     * plug-in a different size and let it lay itself out again, which is what
     * a resizable VST3 does with the window it is given: most of them scale
     * their whole interface to it, and that is close enough to a zoom to be
     * worth wiring the same buttons to.
     *
     * A plug-in that says it cannot resize is left alone and reports false, so
     * the caller can say why the buttons are dead rather than appearing to
     * ignore them. */
    bool setZoom(double z)
    {
        if (!host_ || natW_ <= 0 || natH_ <= 0) return false;
        if (!pehost_editor_can_resize(host_)) return false;
        const int w = int(natW_ * z + 0.5), h = int(natH_ * z + 0.5);
        setMinimumSize(1, 1);
        resize(w, h);            /* resizeEvent tells the plug-in */
        return true;
    }
    bool canZoom() const
    { return host_ && natW_ > 0 && pehost_editor_can_resize(host_); }

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        QWidget::resizeEvent(e);
        /* Only a plug-in that said it can resize is told about one. The others
         * are handed a size once, at attach, and never hear about it again:
         * Cardinal asserts "pData->view != nullptr" inside its own framework on
         * an unexpected resize and then dereferences it anyway, which is the
         * same reason the GTK window sends none at all. */
        if (host_ && pehost_editor_can_resize(host_))
            pehost_editor_resized(host_, width(), height());
    }

private:
    /* One instance owns the hooks; only one editor is open at a time. */
    static EditorHost *self_;

    void install()
    {
        self_ = this;
        v3_runloop_hooks hk{};
        hk.ud = this;
        hk.add_fd = [](void *ud, void *handler, int fd) {
            static_cast<EditorHost *>(ud)->addFd(handler, fd); };
        hk.del_fd = [](void *ud, void *handler) {
            static_cast<EditorHost *>(ud)->delFd(handler); };
        hk.add_timer = [](void *ud, void *handler, unsigned long long ms) {
            static_cast<EditorHost *>(ud)->addTimer(handler, ms); };
        hk.del_timer = [](void *ud, void *handler) {
            static_cast<EditorHost *>(ud)->delTimer(handler); };
        hk.resize = [](void *ud, int w, int h) {
            auto *e = static_cast<EditorHost *>(ud);
            /* The plug-in asking for a size is the plug-in's own idea of its
             * natural one, so it replaces what attach recorded -- otherwise a
             * later zoom would scale from a size the plug-in has moved on from. */
            e->natW_ = w; e->natH_ = h;
            e->setMinimumSize(1, 1);
            e->resize(w, h);
        };
        v3_set_runloop_hooks(&hk);
    }

    /* Retiring rather than deleting, because a plugin routinely unregisters
     * from inside the very callback being dispatched -- dismissing a popup menu
     * is exactly that -- and destroying the notifier there leaves Qt returning
     * through an object it has already freed. Disabling stops any further
     * callback at once; the delete happens when the event loop is back at the
     * top and nothing is on the stack. */
    static void retire(QSocketNotifier *n) { n->setEnabled(false); n->deleteLater(); }
    static void retire(QTimer *t)          { t->stop();            t->deleteLater(); }

    /* One handler, any number of descriptors.
     *
     * IRunLoop keys a registration by handler and says nothing about the
     * descriptor being unique, and a plugin opening a menu uses that: it takes a
     * second X11 connection for the menu window and registers the same handler
     * on it. A plain QHash quietly replaced the first notifier, which then stayed
     * alive, enabled and unreachable -- clearWatches() could not see it to clean
     * it up, so it went on calling a handler the plugin had long since freed.
     * That is a crash on the *next* plugin, with nothing in the log to connect
     * it to the one that opened a menu. */
    void addFd(void *handler, int fd)
    {
        /* Re-registering a descriptor already watched under this handler would
         * otherwise leave Qt with two notifiers on one socket -- which it warns
         * about, and which delivers every event twice. */
        for (QSocketNotifier *old : fds_.values(handler))
            if (old->socket() == qintptr(fd)) { retire(old); fds_.remove(handler, old); break; }

        auto *n = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        connect(n, &QSocketNotifier::activated, this, [handler](QSocketDescriptor d, QSocketNotifier::Type) {
            v3_runloop_fd(handler, int(qintptr(d)));
        });
        fds_.insert(handler, n);
    }
    /* unregisterEventHandler names only the handler, so it retires every
     * descriptor registered under it. */
    void delFd(void *handler)
    {
        for (QSocketNotifier *n : fds_.values(handler)) retire(n);
        fds_.remove(handler);
    }
    void addTimer(void *handler, unsigned long long ms)
    {
        auto *t = new QTimer(this);
        t->setInterval(int(ms ? ms : 16));
        connect(t, &QTimer::timeout, this, [handler] { v3_runloop_timer(handler); });
        t->start();
        timers_.insert(handler, t);
    }
    void delTimer(void *handler)
    {
        for (QTimer *t : timers_.values(handler)) retire(t);
        timers_.remove(handler);
    }
    void clearWatches()
    {
        for (QSocketNotifier *n : fds_)  retire(n);
        for (QTimer *t : timers_)        retire(t);
        fds_.clear();
        timers_.clear();
    }

    pehost *host_ = nullptr;
    QMultiHash<void *, QSocketNotifier *> fds_;
    QMultiHash<void *, QTimer *>          timers_;
public:
    /* What the plug-in laid itself out at, which is what a zoom is a multiple
     * of. Public because the run-loop resize hook is a plain lambda. */
    int natW_ = 0, natH_ = 0;
};
EditorHost *EditorHost::self_ = nullptr;

/* Shows a Windows plugin's editor. The plugin renders into a buffer the Win32
 * layer owns; this widget blits it and turns Qt input back into WM_* messages.
 * Unlike the X11 path there is no child window -- everything is pixels. */
class PixelEditor : public QWidget {
    Q_OBJECT
signals:
    /* Ctrl and the wheel, which the zoom bar answers. Not handled here: the
     * window owns the zoom, because the same buttons drive the native editor
     * as well and one of the two has to be in charge. */
    void zoomStep(int dir);

public:
    explicit PixelEditor(QWidget *p = nullptr) : QWidget(p)
    {
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setMinimumSize(200, 120);
        pump_ = new QTimer(this);
        connect(pump_, &QTimer::timeout, this, &PixelEditor::tick);
    }

    bool attach(pehost *h)
    {
        detach();
        if (!h || pehost_editor_kind(h) != PEHOST_EDITOR_PIXELS) return false;
        int w = 0, ht = 0;
        pehost_editor_size(h, &w, &ht);
        if (w > 0 && ht > 0) { natW_ = w; natH_ = ht; applySize(); }
        if (pehost_editor_open(h) != 0) {
            fprintf(stderr, "pestudio: editor_open refused\n"); fflush(stderr);
            return false;
        }
        host_ = h;
        reported_ = false;
        pump_->start(16);
        return true;
    }
    void detach()
    {
        pump_->stop();
        host_ = nullptr;
        natW_ = natH_ = 0;
        img_ = QImage();
        update();
    }
    bool attached() const { return host_ != nullptr; }

    /* Scale the blit. The plug-in knows nothing about it: it goes on drawing at
     * its own size into its own buffer, and only the last step -- painting that
     * buffer onto this widget -- changes. Input is mapped back the other way in
     * send(), so the plug-in still receives the coordinates it drew at. */
    bool setZoom(double z)
    {
        if (natW_ <= 0 || natH_ <= 0) return false;
        zoom_ = z;
        applySize();
        update();
        return true;
    }
    bool canZoom() const { return natW_ > 0; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter g(this);
        if (img_.isNull()) { g.fillRect(rect(), palette().window()); return; }
        if (zoom_ == 1.0) { g.drawImage(0, 0, img_); return; }
        /* Smoothed only when zooming out, which is the direction that actually
         * needs it -- dropping every other pixel of a knob leaves it ragged.
         * Enlarging is left sharp: an editor is a grid of hard-edged artwork
         * and text, and blurring it up is worse than seeing the pixels. */
        g.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 1.0);
        g.drawImage(QRect(0, 0, scaled().width(), scaled().height()), img_);
    }

    /* WM_* mouse codes, so the plugin sees what it expects. */
    enum { WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202,
           WM_LBUTTONDBLCLK = 0x0203, WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205,
           WM_MBUTTONDOWN = 0x0207, WM_MBUTTONUP = 0x0208, WM_MOUSEWHEEL = 0x020A };

    static int mkButtons(Qt::MouseButtons b)
    {
        int m = 0;
        if (b & Qt::LeftButton)   m |= 0x0001;   /* MK_LBUTTON */
        if (b & Qt::RightButton)  m |= 0x0002;   /* MK_RBUTTON */
        if (b & Qt::MiddleButton) m |= 0x0010;   /* MK_MBUTTON */
        return m;
    }
    /* Every mouse message goes through here, which is what makes zoom safe to
     * add: the plug-in is told where the click landed in its own picture, not
     * where it landed on screen. Getting this wrong does not look like a bug in
     * the zoom -- it looks like the plug-in's knobs have stopped working. */
    void send(int msg, QPoint p, Qt::MouseButtons b, int wheel = 0)
    {
        if (host_) {
            PluginCall guard;
            const QPoint q = zoom_ == 1.0
                ? p : QPoint(int(p.x() / zoom_), int(p.y() / zoom_));
            pehost_editor_mouse(host_, q.x(), q.y(), msg, mkButtons(b), wheel);
        }
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        setFocus();
        send(e->button() == Qt::RightButton ? WM_RBUTTONDOWN
           : e->button() == Qt::MiddleButton ? WM_MBUTTONDOWN : WM_LBUTTONDOWN,
             e->position().toPoint(), e->buttons());
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        send(e->button() == Qt::RightButton ? WM_RBUTTONUP
           : e->button() == Qt::MiddleButton ? WM_MBUTTONUP : WM_LBUTTONUP,
             e->position().toPoint(), e->buttons());
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    { send(WM_LBUTTONDBLCLK, e->position().toPoint(), e->buttons()); }
    void mouseMoveEvent(QMouseEvent *e) override
    { send(WM_MOUSEMOVE, e->position().toPoint(), e->buttons()); }
    void wheelEvent(QWheelEvent *e) override
    {
        /* Ctrl and the wheel is the zoom everywhere else, and the plug-in is
         * not expecting it -- a bare wheel still belongs to whatever control is
         * under the pointer, which is the only way to work some editors. */
        if (e->modifiers() & Qt::ControlModifier) {
            const int d = e->angleDelta().y();
            if (d) emit zoomStep(d > 0 ? 1 : -1);
            e->accept();
            return;
        }
        send(WM_MOUSEWHEEL, e->position().toPoint(), e->buttons(),
             e->angleDelta().y() / 120);
    }
    void keyPressEvent(QKeyEvent *e) override
    {
        if (!host_) return;
        const QString t = e->text();
        pehost_editor_key(host_, qtToVk(e->key()), 1,
                          t.isEmpty() ? 0 : t.at(0).unicode());
    }
    void keyReleaseEvent(QKeyEvent *e) override
    { if (host_) pehost_editor_key(host_, qtToVk(e->key()), 0, 0); }

private:
    /* Qt key codes to Windows virtual keys, for the ones an editor cares about. */
    static int qtToVk(int k)
    {
        switch (k) {
        case Qt::Key_Backspace: return 0x08;
        case Qt::Key_Tab:       return 0x09;
        case Qt::Key_Return: case Qt::Key_Enter: return 0x0D;
        case Qt::Key_Escape:    return 0x1B;
        case Qt::Key_Delete:    return 0x2E;
        case Qt::Key_Left:      return 0x25;
        case Qt::Key_Up:        return 0x26;
        case Qt::Key_Right:     return 0x27;
        case Qt::Key_Down:      return 0x28;
        default:
            if (k >= Qt::Key_A && k <= Qt::Key_Z) return k;          /* already VK_A.. */
            if (k >= Qt::Key_0 && k <= Qt::Key_9) return k;
            return k & 0xFF;
        }
    }

    void tick()
    {
        if (!host_ || g_inPlugin) return;   /* already inside the plugin */
        PluginCall guard;
        pehost_editor_pump(host_);
        const unsigned int *px = nullptr;
        int w = 0, h = 0;
        if (!pehost_editor_pixels(host_, &px, &w, &h) || !px || w <= 0 || h <= 0) {
            /* Say so once per editor rather than once per process: a plugin whose
             * editor never produces a frame is exactly what wants reporting, and
             * a process-wide flag hides every case after the first. */
            if (!reported_) { reported_ = true;
                fprintf(stderr, "pestudio: editor produced no pixels\n"); fflush(stderr); }
            return;
        }
        if (!reported_) { reported_ = true;
            fprintf(stderr, "pestudio: editor pixels %dx%d\n", w, h); fflush(stderr); }
        /* The buffer is 32-bit BGRX top-down, which is exactly Format_RGB32 on a
         * little-endian machine, so this wraps rather than converts. */
        img_ = QImage(reinterpret_cast<const uchar *>(px), w, h,
                      w * 4, QImage::Format_RGB32).copy();
        /* PESTUDIO_DUMP=<dir> writes what this widget is about to paint. The host
         * returning good pixels and the window showing them are two different
         * claims, and only this checks the second. */
        if (!dumped_ && qEnvironmentVariableIsSet("PESTUDIO_DUMP")) {
            dumped_ = true;
            QString d = qEnvironmentVariable("PESTUDIO_DUMP");
            QString nm = QString("%1/%2.png").arg(d).arg(dumpName_);
            if (img_.save(nm)) { fprintf(stderr, "pestudio: painted -> %s\n",
                                         qPrintable(nm)); fflush(stderr); }
        }
        /* The plug-in may have changed its own size under us -- opening a
         * larger panel, switching skin. Scale from whatever it is drawing now. */
        if (w != natW_ || h != natH_) { natW_ = w; natH_ = h; applySize(); }
        update();
    }

    QSize scaled() const
    { return QSize(int(natW_ * zoom_ + 0.5), int(natH_ * zoom_ + 0.5)); }

    void applySize()
    {
        const QSize s = scaled();
        if (s.width() <= 0 || s.height() <= 0) return;
        setMinimumSize(s);
        resize(s);
    }

    pehost *host_ = nullptr;
    QTimer *pump_ = nullptr;
    QImage  img_;
    int     natW_ = 0, natH_ = 0;   /* what the plug-in draws at */
    double  zoom_ = 1.0;
    bool    reported_ = false;
public:
    bool    dumped_ = false;
    QString dumpName_;
};

/* -------------------------------------------------------------- main window */

class Window : public QMainWindow {
    Q_OBJECT
public:
    /* Everything holding the plugin has to let go here, before anything else is
     * torn down.
     *
     * C++ destroys this object's members -- the Engine among them, which closes
     * the plugin -- before ~QObject deletes its children. Anything that is both
     * a child and a holder of the pehost pointer therefore outlives what it
     * points at, and this destructor body is the only place that ordering can be
     * fixed.
     *
     * The MIDI reader thread was the first case: it called into the Engine after
     * the Engine was gone, and closing the window crashed. EditorHost is the
     * same fault in slower motion -- ~EditorHost calls detach(), which calls
     * pehost_editor_detach() on the plugin the Engine has already freed. It
     * shows on any orderly exit with a native Linux VST3 editor open, --cycle
     * included. */
    ~Window() override
    {
        if (midi_)        midi_->stopInput();
        if (editor_)      editor_->detach();
        if (pixelEditor_) pixelEditor_->detach();
        patch_bank_free(bank_);
        patch_bank_free(autoBank_);
    }

    /* --cycle walks every plugin in the list, opening each editor in turn.
     * Switching plugins with editors attached is the failure-prone path and
     * clicking through it by hand is not repeatable. */
    void startCycle(int ms)
    {
        cycleTimer_ = new QTimer(this);
        connect(cycleTimer_, &QTimer::timeout, this, [this] {
            int n = pluginList_->count();
            if (!n) return;
            int next = pluginList_->currentRow() + 1;
            if (next >= n) {
                fprintf(stderr, "pestudio: cycle complete, %d plugins\n", n);
                fflush(stderr);
                cycleTimer_->stop();
                QCoreApplication::quit();
                return;
            }
            fprintf(stderr, "pestudio: cycle -> row %d\n", next);
            fflush(stderr);
            pluginList_->setCurrentRow(next);
            /* Selecting a row only loads the plugin; the editor is instantiated
             * when its tab is shown. Without this the cycle never opened one,
             * which made it useless for the thing it exists to test. */
            if (tabs_->isTabEnabled(1)) tabs_->setCurrentIndex(1);
        });
        /* The first row is already loaded by the time a cycle starts, so open its
         * editor too -- otherwise row 0 is the one plugin the sweep never tests. */
        if (tabs_->isTabEnabled(1)) tabs_->setCurrentIndex(1);
        cycleTimer_->start(ms);
    }

    /* `startPlugin` names one plugin to open on launch and `bank` a set of
     * patches to offer for it, so a session can be reproduced from a command
     * line rather than clicked back together. `startDir` is the browsing root as
     * before; a named plugin implies its own directory, so the two are not both
     * needed. Ownership of `bank` passes to the window. */
    explicit Window(const QString &startDir = QString(),
                    const QString &startPlugin = QString(),
                    const QString &bankFile = QString(),
                    patch_bank *bank = nullptr, int startPatch = 0)
        : startPlugin_(startPlugin), bankFile_(bankFile), bank_(bank),
          startPatch_(startPatch)
    {
        setWindowTitle("pestudio -- native Windows VST2 host");
        resize(1180, 760);

        auto *split = new QSplitter(this);

        /* left: plugin browser + programs */
        auto *left = new QWidget;
        auto *lv = new QVBoxLayout(left);
        lv->setContentsMargins(6, 6, 6, 6);

        rootBox_ = new QComboBox;
        for (const auto &r : discoverRoots()) rootBox_->addItem(r.first, r.second);
        loadUserRoots();   /* the folders the user added in past sessions */
        auto *dirRow = new QHBoxLayout;
        /* A named plugin sets the browsing root to the directory holding it, so
         * the list it lands in is the one it came from and its neighbours are
         * there to switch to. */
        dirEdit_ = new QLineEdit(
            !startPlugin.isEmpty() ? QFileInfo(startPlugin).absolutePath()
            : !startDir.isEmpty()  ? startDir
            : rootBox_->count()    ? rootBox_->itemData(0).toString()
                                   : defaultDir());
        auto *browse = new QPushButton("...");
        browse->setFixedWidth(30);
        dirRow->addWidget(new QLabel("Dir:"));
        dirRow->addWidget(dirEdit_);
        dirRow->addWidget(browse);
        lv->addWidget(rootBox_);
        lv->addLayout(dirRow);

        /* Opening a plug-in and adding a folder used to be a pair of buttons
         * here. They are File menu items now -- see buildMenus() -- because
         * that is where a program's file commands belong, and because the same
         * two commands then sit in the same place in both windows. */

        /* Force a loader when auto-detect guesses wrong -- a Windows VST3 shipped
         * as a bare .dll, a plugin whose bytes and name disagree. "Auto-detect"
         * (the default) sniffs, exactly as before; any other choice hands the
         * file straight to that backend so the user can try it. */
        auto *forceRow = new QHBoxLayout;
        forceBox_ = new QComboBox;
        forceBox_->addItem("Auto-detect", (int)PEHOST_KIND_AUTO);
        {
            static const pehost_kind kinds[] = {
                PEHOST_KIND_WIN_VST2_64, PEHOST_KIND_WIN_VST2_32, PEHOST_KIND_WIN_VST3,
                PEHOST_KIND_LINUX_VST3,  PEHOST_KIND_LINUX_VST2,
                PEHOST_KIND_MAC_VST2,    PEHOST_KIND_MAC_VST3,   PEHOST_KIND_MAC_AU,
                PEHOST_KIND_CLASSIC_MAC,
            };
            for (pehost_kind k : kinds)
                forceBox_->addItem(QString::fromUtf8(pehost_kind_label(k)), (int)k);
        }
        forceRow->addWidget(new QLabel("Load as:"));
        forceRow->addWidget(forceBox_, 1);
        lv->addLayout(forceRow);

        pluginList_ = new QListWidget;
        lv->addWidget(new QLabel("Plugins"));
        lv->addWidget(pluginList_, 3);

        programList_ = new QListWidget;
        lv->addWidget(new QLabel("Programs"));
        lv->addWidget(programList_, 4);

        /* The patches a bank file offers, shown only when one was loaded --
         * browsing without a bank should look exactly as it did before. */
        patchLabel_ = new QLabel("Patches");
        patchList_  = new QListWidget;
        lv->addWidget(patchLabel_);
        lv->addWidget(patchList_, 3);
        /* Filled by rebuildPatchList() once a plugin is loaded, because which
         * patches belong depends on which plugin that is. */
        patchLabel_->setVisible(bank_ != nullptr);
        patchList_->setVisible(bank_ != nullptr);
        if (bank_) {
            patchLabel_->setText(QString("Patches -- %1")
                                     .arg(QFileInfo(bankFile_).fileName()));
            patchLabel_->setToolTip(bankFile_);
        }

        /* MIDI: a tracker or USB keyboard connects to "pestudio in", and
         * anything played here is echoed to "pestudio out". */
        auto *midiBox = new QGroupBox("MIDI");
        auto *mg = new QGridLayout(midiBox);
        midiPort_ = new QLabel("-");
        midiPort_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        midiChan_ = new QComboBox;
        midiChan_->addItem("All channels", -1);
        for (int c = 0; c < 16; c++) midiChan_->addItem(QString("Channel %1").arg(c + 1), c);
        midiThru_ = new QCheckBox("Thru (in -> out)");
        midiOutAuto_ = new QCheckBox("Connect out to hardware");
        auto *rescanBtn = new QPushButton("Rescan");
        /* Tempo, for when nothing is sending clock. A plugin with a synced delay
         * or arpeggiator has to be told the tempo by someone; if no sequencer is
         * driving, this is the only way to say it. When clock *is* arriving the
         * box follows it rather than fighting it. */
        tempoBox_ = new QDoubleSpinBox;
        tempoBox_->setRange(20.0, 999.0);
        tempoBox_->setDecimals(2);
        tempoBox_->setValue(120.0);
        tempoBox_->setSuffix(" BPM");
        tempoBox_->setKeyboardTracking(false);
        tempoSync_ = new QLabel("internal");
        tempoSync_->setStyleSheet("color:#888");
        midiSources_ = new QLabel("not open");
        midiSources_->setWordWrap(true);
        midiSources_->setStyleSheet("color:#888");
        mg->addWidget(new QLabel("Port:"), 0, 0);
        mg->addWidget(midiPort_,           0, 1);
        mg->addWidget(rescanBtn,           0, 2);
        mg->addWidget(midiChan_,           1, 0, 1, 3);
        mg->addWidget(midiThru_,           2, 0, 1, 3);
        mg->addWidget(midiOutAuto_,        3, 0, 1, 3);
        mg->addWidget(new QLabel("Tempo:"),  4, 0);
        mg->addWidget(tempoBox_,             4, 1);
        mg->addWidget(tempoSync_,            4, 2);
        mg->addWidget(midiSources_,          5, 0, 1, 3);
        lv->addWidget(midiBox);
        left->setMinimumWidth(300);

        /* right: info + parameters */
        auto *right = new QWidget;
        auto *rv = new QVBoxLayout(right);
        rv->setContentsMargins(6, 6, 6, 6);

        info_ = new QLabel("No plugin loaded.");
        info_->setTextFormat(Qt::RichText);
        info_->setWordWrap(true);
        rv->addWidget(info_);

        paramModel_ = new ParamModel;
        paramTable_ = new ParamTable;
        paramTable_->setModel(paramModel_);
        paramTable_->setItemDelegate(new BarDelegate(paramTable_));
        paramTable_->setSelectionMode(QAbstractItemView::NoSelection);
        paramTable_->setShowGrid(false);
        paramTable_->setAlternatingRowColors(true);
        paramTable_->verticalHeader()->setDefaultSectionSize(22);
        paramTable_->verticalHeader()->setVisible(false);
        paramTable_->horizontalHeader()->setStretchLastSection(false);
        paramTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        paramTable_->setMouseTracking(true);

        editor_ = new EditorHost;
        pixelEditor_ = new PixelEditor;
        editorStack_ = new QStackedWidget;
        editorStack_->addWidget(editor_);        /* 0: X11 embed  */
        editorStack_->addWidget(pixelEditor_);   /* 1: pixel blit */
        editorScroll_ = new QScrollArea;
        editorScroll_->setWidget(editorStack_);
        editorScroll_->setWidgetResizable(false);
        editorScroll_->setAlignment(Qt::AlignCenter);

        /* The zoom bar.
         *
         * Plug-in editors are drawn at a size the plug-in chose, and several in
         * this corpus are taller than the screen -- scrolling a synth you are
         * trying to play is not much of an answer, so the picture is scaled to
         * the room available instead. Above the viewport rather than floating
         * over it: a foreign X11 child sits on top of everything Qt paints, so
         * anything overlaid on the editor would be invisible and unclickable
         * exactly when it is a native editor that needs it. */
        zoomOut_  = new QToolButton; zoomOut_->setText("\xe2\x88\x92");
        zoomIn_   = new QToolButton; zoomIn_->setText("+");
        zoomFit_  = new QPushButton("Fit");
        zoom1to1_ = new QPushButton("1:1");
        zoomLabel_ = new QLabel("100%");
        zoomLabel_->setMinimumWidth(48);
        zoomLabel_->setAlignment(Qt::AlignCenter);
        zoomNote_ = new QLabel;
        zoomNote_->setStyleSheet("color:#888");
        zoomOut_->setToolTip("Zoom out  (Ctrl+wheel over the editor)");
        zoomIn_->setToolTip("Zoom in  (Ctrl+wheel over the editor)");
        zoomFit_->setToolTip("Scale the editor to fit the space there is");
        zoom1to1_->setToolTip("Back to the size the plug-in drew");
        /* Not in the tab order: Tab is how you get from the plug-in list to the
         * keyboard, and four more stops on the way is four more chances to be
         * typing at something that is not the synth. */
        for (QWidget *w : { (QWidget *)zoomOut_, (QWidget *)zoomIn_,
                            (QWidget *)zoomFit_, (QWidget *)zoom1to1_ })
            w->setFocusPolicy(Qt::NoFocus);
        auto *zoomBar = new QHBoxLayout;
        zoomBar->setContentsMargins(2, 2, 2, 2);
        zoomBar->addWidget(new QLabel("Zoom"));
        zoomBar->addWidget(zoomOut_);
        zoomBar->addWidget(zoomLabel_);
        zoomBar->addWidget(zoomIn_);
        zoomBar->addWidget(zoomFit_);
        zoomBar->addWidget(zoom1to1_);
        zoomBar->addWidget(zoomNote_, 1);
        auto *editorPage = new QWidget;
        auto *ev = new QVBoxLayout(editorPage);
        ev->setContentsMargins(0, 0, 0, 0);
        ev->addLayout(zoomBar);
        ev->addWidget(editorScroll_, 1);

        tabs_ = new QTabWidget;
        tabs_->addTab(paramTable_, "Parameters");
        tabs_->addTab(editorPage, "Editor");
        rv->addWidget(tabs_, 1);

        split->addWidget(left);
        split->addWidget(right);
        split->setStretchFactor(1, 1);

        /* bottom: transport + keyboard */
        auto *central = new QWidget;
        auto *cv = new QVBoxLayout(central);
        cv->setContentsMargins(0, 0, 0, 0);
        cv->addWidget(split, 1);

        auto *bar = new QHBoxLayout;
        auto *panic = new QPushButton("All notes off");
        recBtn_ = new QPushButton("● Record");
        recBtn_->setToolTip("record what you play to a WAV in renders/");
        recLabel_ = new QLabel;
        recLabel_->setMinimumWidth(230);
        gain_ = new QSlider(Qt::Horizontal);
        gain_->setRange(0, 150);
        gain_->setValue(80);
        gain_->setFixedWidth(140);
        level_ = new QProgressBar;
        level_->setRange(0, 100);
        level_->setTextVisible(false);
        level_->setFixedWidth(160);
        /* What an effect is fed. Silence is right for a synth and useless for an
         * effect, so the choice is exposed rather than assumed. */
        srcBox_ = new QComboBox;
        srcBox_->addItem("silence", int(Engine::SrcSilence));
        srcBox_->addItem("keys",    int(Engine::SrcNotes));
        srcBox_->addItem("noise",   int(Engine::SrcNoise));
        srcBox_->setToolTip("what to feed an effect's input -- a synth ignores it");
        bar->addWidget(panic);
        bar->addWidget(recBtn_);
        bar->addWidget(recLabel_);
        bar->addStretch(1);
        bar->addWidget(new QLabel("Effect in"));
        bar->addWidget(srcBox_);
        bar->addSpacing(12);
        bar->addWidget(new QLabel("Level"));
        bar->addWidget(level_);
        bar->addWidget(new QLabel("Gain"));
        bar->addWidget(gain_);
        cv->addLayout(bar);

        /* The wheel sits left of the keys, where a keyboard puts it. Same row so
         * it is the same height as them without being told a size. */
        piano_ = new Piano;
        wheel_ = new PitchWheel;
        auto *keys = new QHBoxLayout;
        keys->setContentsMargins(0, 0, 0, 0);
        keys->setSpacing(4);
        keys->addWidget(wheel_);
        keys->addWidget(piano_, 1);
        cv->addLayout(keys);
        setCentralWidget(central);
        buildMenus();
        statusBar()->showMessage("starting audio...");

        /* wiring */
        connect(browse, &QPushButton::clicked, this, [this] {
            QString d = QFileDialog::getExistingDirectory(this, "Plugin directory", dirEdit_->text());
            if (!d.isEmpty()) { dirEdit_->setText(d); rescan(); }
        });
        connect(dirEdit_, &QLineEdit::returnPressed, this, &Window::rescan);
        if (!startDir.isEmpty())
            for (int i = 0; i < rootBox_->count(); i++)
                if (rootBox_->itemData(i).toString() == QDir(startDir).absolutePath()) {
                    QSignalBlocker b(rootBox_);
                    rootBox_->setCurrentIndex(i);
                    break;
                }
        connect(rootBox_, &QComboBox::currentIndexChanged, this, [this](int i) {
            if (i >= 0) { dirEdit_->setText(rootBox_->itemData(i).toString()); rescan(); }
        });
        connect(pluginList_, &QListWidget::currentRowChanged, this, &Window::loadRow);
        /* Changing "Load as" re-opens the current selection under the new loader,
         * so trying a different backend is one click, not a reload dance. Guarded
         * so populating the combo at startup (no selection yet) does nothing. */
        connect(forceBox_, &QComboBox::currentIndexChanged, this, [this] {
            int row = pluginList_->currentRow();
            if (row >= 0) loadRow(row);
        });
        connect(patchList_, &QListWidget::currentRowChanged, this, &Window::applyPatchRow);
        connect(programList_, &QListWidget::currentRowChanged, this, [this](int r) {
            /* A program change dispatches straight into the plugin, so it has
             * the same re-entrancy problem loadRow has. It arrives that way
             * without anybody clicking, too: an incoming MIDI program change is
             * queued to this thread from the reader, and the input pump's
             * processEvents delivers it from inside a plugin's drag loop. */
            if (g_inPlugin) {
                statusBar()->showMessage("finish the gesture before changing program", 2000);
                return;
            }
            if (r >= 0 && eng_.host()) {
                pehost_set_program(eng_.host(), r);
                /* A program change overwrites every parameter, so the patch the
                 * user is listening to would be silently discarded and the
                 * sound would change under them. With a bank loaded the patch
                 * is what defines the sound and the program is only the base it
                 * sits on, so re-assert it. Its own program is *not* forced
                 * back, which would undo the selection just made. Pick the
                 * first row of the Patches list to hear programs on their own. */
                reassertPatch();
                refreshParams();
            }
        });
        connect(zoomOut_,  &QToolButton::clicked,  this, [this] { zoomStep(-1); });
        connect(zoomIn_,   &QToolButton::clicked,  this, [this] { zoomStep(+1); });
        connect(zoomFit_,  &QPushButton::clicked,  this, [this] { zoomFit(); });
        connect(zoom1to1_, &QPushButton::clicked,  this, [this] { setZoom(1.0); });
        connect(pixelEditor_, &PixelEditor::zoomStep, this, &Window::zoomStep);
        connect(recBtn_, &QPushButton::clicked, this, &Window::toggleRecord);
        connect(panic, &QPushButton::clicked, this, [this] {
            if (piano_) piano_->releaseAll();
            if (eng_.host()) pehost_all_notes_off(eng_.host());
            eng_.allNotesOff();
            if (midi_) midi_->send(0xB0, 123, 0);
            for (int n = 0; n < 128; n++) piano_->setHeld(n, false);
            /* A bend the plugin still thinks is applied survives every note
             * being cut, and then the next thing played is in the wrong key. */
            if (wheel_) wheel_->recentre();
        });
        connect(gain_, &QSlider::valueChanged, this, [this](int v) { eng_.gain_ = v / 100.0f; });
        connect(srcBox_, &QComboBox::currentIndexChanged, this, [this](int i) {
            eng_.setSource(Engine::Source(srcBox_->itemData(i).toInt()));
        });
        /* Watch keys for the whole application, so a note key pressed with the
         * piano focused is still released when the key comes up over the editor.
         * A key that played a note is consumed; everything else is passed on,
         * and over the plug-in's editor nothing is consumed at all -- see
         * eventFilter. */
        qApp->installEventFilter(this);

        connect(piano_, &Piano::noteOn, this, [this](int n, int v) {
            if (eng_.host()) pehost_note_on(eng_.host(), n, v);
            /* The keys also play the internal source, which is what an effect
             * hears. A synth ignores it because nothing is fed to a synth. */
            eng_.noteOn(n, v);
            if (midi_) midi_->send(0x90, n, v);
        });
        connect(piano_, &Piano::noteOff, this, [this](int n) {
            if (eng_.host()) pehost_note_off(eng_.host(), n);
            eng_.noteOff(n);
            if (midi_) midi_->send(0x80, n, 0);
        });
        /* Bend goes the same two places a note does: into the plugin and out of
         * the port. The internal source is left out on purpose -- it is a gate
         * per note for feeding an effect, with no pitch to bend. */
        connect(wheel_, &PitchWheel::bend, this, [this](int v14) {
            const int lsb = v14 & 0x7f, msb = (v14 >> 7) & 0x7f;
            if (eng_.host()) pehost_midi(eng_.host(), 0xE0, lsb, msb);
            if (midi_) midi_->send(0xE0, lsb, msb);
        });

        /* MIDI in/out. Notes drive both the plugin and the output port, so
         * pestudio can play external gear as well as be played by it. */
        midi_ = new MidiIo(this);
        /* The plugin is fed from the reader thread, not from here: going through
         * the Qt event loop puts every note behind whatever the interface is
         * doing. pehost's event queue is lock-free and made for this. The signal
         * below still runs on the GUI thread, but only for things that may lag a
         * frame without anyone hearing it. */
        midi_->setRealtimeSink([this](int st, int d1, int d2) {
            eng_.withHost([&](pehost *h) { pehost_midi(h, st, d1, d2); });
        });
        QString mErr;
        if (!midi_->open(&mErr)) {
            midiSources_->setText("unavailable: " + mErr);
        } else {
            /* The ALSA address alone ("128:0") is not what a tracker's device
             * list shows -- it shows the client name. Say both, so what is on
             * screen here can be matched against what is in the dropdown
             * there, and so a second instance is visibly "pestudio 2". */
            midiPort_->setText(QString("%1  (%2 in)")
                                   .arg(midi_->portName(), midi_->clientName()));
            updateMidiSources();
        }
        connect(midi_, &MidiIo::connectionsChanged, this, &Window::updateMidiSources);
        connect(tabs_, &QTabWidget::currentChanged, this, [this](int i) {
            if (i == 1) openEditor();
        });
        connect(rescanBtn, &QPushButton::clicked, this, [this] {
            int n = midi_->rescan();
            statusBar()->showMessage(n ? QString("MIDI: %1 new connection(s)").arg(n)
                                      : QString("MIDI: no new sources"), 4000);
        });
        connect(midiChan_, &QComboBox::currentIndexChanged, this, [this](int) {
            midi_->setChannelFilter(midiChan_->currentData().toInt());
        });
        connect(midiThru_, &QCheckBox::toggled, this, [this](bool on) { midi_->setThru(on); });
        connect(tempoBox_, &QDoubleSpinBox::valueChanged, this, [this](double bpm) {
            /* Only when the user typed it: echoing back a value that came from
             * the clock would fight the sync. */
            if (tempoFromClock_) return;
            if (eng_.host()) pehost_set_tempo(eng_.host(), bpm, 4, 4);
        });
        connect(midiOutAuto_, &QCheckBox::toggled, this, [this](bool on) {
            midi_->setAutoConnectOut(on);
            if (on) midi_->rescan();
        });
        /* Raw MIDI goes straight to the plugin so wheels and pedals work. */
        connect(midi_, &MidiIo::midi, this, [this](int st, int d1, int d2) {
            if (st == 0xF8) clockSeen_ = true;
            /* A hardware wheel moves the one on screen, the way incoming notes
             * light the keys. setBend and not the signal: sending it on would
             * put it back out of the port it arrived from. */
            if ((st & 0xf0) == 0xE0 && wheel_) wheel_->setBend((d2 << 7) | d1);
        });
        connect(midi_, &MidiIo::noteOn,  this, [this](int n, int) { piano_->setHeld(n, true); });
        connect(midi_, &MidiIo::noteOff, this, [this](int n) { piano_->setHeld(n, false); });
        connect(midi_, &MidiIo::programChange, this, [this](int p) {
            if (programList_->count())
                programList_->setCurrentRow(p % programList_->count());
        });

        /* Parameter displays are computed by the plugin, so poll them rather
         * than trying to predict the formatting. */
        auto *tick = new QTimer(this);
        connect(tick, &QTimer::timeout, this, &Window::pollUi);
        tick->start(80);

        QString err;
        if (!eng_.startAudio(&err))
            statusBar()->showMessage("audio failed: " + err);
        else
            statusBar()->showMessage("pipewire, " + QString::number(kQuantum) +
                                     "-frame quantum (" +
                                     QString::number(1000.0 * kQuantum / kSampleRate, 'f', 1) +
                                     " ms), realtime");
        /* If a previous run died mid-render, that plugin is suspect. */
        {
            QFile f(markerPath());
            if (f.open(QIODevice::ReadOnly)) {
                crashed_ = QString::fromUtf8(f.readAll()).trimmed();
                f.close();
                QFile::remove(markerPath());
            }
        }
        rescan();
        /* Only report a skip when one really happened. A plugin named on the
         * command line is loaded even if it is the one that died last time, and
         * saying "skipped" about the plugin now on screen is simply wrong. */
        const int cur = pluginList_->currentRow();
        const bool loadedTheSuspect = cur >= 0 && cur < paths_.size() &&
                                      paths_[cur] == crashed_;
        if (!crashed_.isEmpty() && !loadedTheSuspect) {
            QString base = QFileInfo(crashed_).fileName();
            statusBar()->showMessage("skipped " + base +
                                     " -- it did not survive the last session; select it to retry", 0);
            /* Only take over the info panel if nothing else loaded, otherwise
             * the notice hides the plugin the user is actually looking at. */
            if (!eng_.host())
                info_->setText("<b>" + base.toHtmlEscaped() + "</b> did not survive the last "
                               "session and was not auto-loaded.<br><span style='color:#888'>"
                               "Select it in the list to try again.</span>");
        }
        piano_->setFocus();
    }

protected:
    /* Reaching here means we exited under our own power, so whatever is loaded
     * is exonerated. Anything that kills the process instead leaves the marker
     * behind, which is exactly the signal we want. Qt closes every window on
     * quit(), so --cycle's unattended finish comes through here too. */
    void closeEvent(QCloseEvent *e) override
    {
        QFile::remove(markerPath());
        QMainWindow::closeEvent(e);
    }

private slots:
    void rescan()
    {
        pluginList_->clear();
        paths_ = QStringList();
        unloadable_.clear();

        /* Walk the tree rather than one flat directory: VST2 plugins are loose
         * .dll files while VST3 arrives either as a single file or as a bundle
         * directory, and the Linux builds sit several levels down. A .vst3
         * directory is a leaf -- its innards are not separate plugins. */
        QDir root(dirEdit_->text());
        QList<QPair<QString, QString>> found;   /* label, absolute path */
        QStringList queue{ root.absolutePath() };
        int guard = 0;
        while (!queue.isEmpty() && guard++ < 4000) {
            QDir d(queue.takeFirst());
            for (const QFileInfo &fi : d.entryInfoList(QDir::Files | QDir::Dirs |
                                                       QDir::NoDotAndDotDot,
                                                       QDir::Name | QDir::IgnoreCase)) {
                const QString nm = fi.fileName();
                const bool isV3 = nm.endsWith(".vst3", Qt::CaseInsensitive);
                /* The export decides, not the extension. A .dll is only a
                 * Windows binary -- an installer's setup.dll ends in .dll too,
                 * and used to be listed beside the synthesisers and fail when
                 * picked. Same reasoning already applied to .so below. */
                const bool isV2 = fi.isFile() &&
                    nm.endsWith(".dll", Qt::CaseInsensitive) &&
                    pehost_is_windows_vst(
                        fi.absoluteFilePath().toLocal8Bit().constData());
                /* A macOS plugin is a bundle directory, so it is a candidate in
                 * its own right rather than something to walk into. */
                /* Not offered while PESTUDIO_MAC is off -- see the note at the
                 * top. A .vst bundle left in a scanned directory is passed over
                 * as if it were any other folder, so browsing cannot reach one.
                 * The header test for a Classic plug-in is skipped too, which
                 * also spares every unrecognised file in the tree from being
                 * opened and sniffed. */
                const bool isMac = PESTUDIO_MAC && fi.isDir() &&
                                   (nm.endsWith(".vst", Qt::CaseInsensitive) ||
                                    nm.endsWith(".component", Qt::CaseInsensitive));
                /* A Classic Mac OS plugin is a plain file -- a PEF, or a resource
                 * fork carrying one -- with no extension convention worth
                 * trusting, so the header decides rather than the name. */
                const bool isClassic = PESTUDIO_MAC && fi.isFile() && !isV2 &&
                    pehost_is_classic_mac(
                        fi.absoluteFilePath().toLocal8Bit().constData());
                /* A native Linux VST2 is a bare .so, so the export decides -- a
                 * plain ELF check would also match every support library and an
                 * LV2 bundle's inner .so. Those inner ones are skipped by path
                 * as well, to avoid dlopen'ing a library that is not a candidate
                 * just to find out it is not one. */
                const bool inLv2 = fi.absoluteFilePath().contains(".lv2/");
                const bool isLinuxV2 = fi.isFile() && !inLv2 &&
                    nm.endsWith(".so", Qt::CaseInsensitive) &&
                    pehost_is_native_vst2(
                        fi.absoluteFilePath().toLocal8Bit().constData());
                if (isV3 || isV2 || isMac || isClassic || isLinuxV2) {
                    /* Check the header now rather than discovering at load time
                     * that it is, say, a 32-bit build this loader cannot run. */
                    /* Room for a real explanation: "Classic Mac OS / Carbon
                     * (CFM/PEF, PowerPC)" is worth showing in full. */
                    char why[128] = { 0 };
                    const bool ok = pehost_can_load(
                        fi.absoluteFilePath().toLocal8Bit().constData(), why, sizeof why);
                    /* Say when a plugin will be hosted out of process: it
                     * behaves the same, but a crash there is survivable and
                     * worth distinguishing when one happens. */
                    const bool bridged = ok && pehost_is_bridged(
                        fi.absoluteFilePath().toLocal8Bit().constData());
                    QString label = nm + (isV3 ? "   [VST3]"
                                              : isClassic ? "   [Mac OS 9 VST]"
                                              : isMac ? (nm.endsWith(".component", Qt::CaseInsensitive)
                                                         ? "   [macOS AU]" : "   [macOS VST2]")
                                              : isLinuxV2 ? "   [Linux VST2]"
                                              : bridged ? "   [VST2 32-bit]" : "   [VST2]");
                    if (!ok) label += QString("  -- %1").arg(why[0] ? why : "unsupported");
                    found << qMakePair(label, fi.absoluteFilePath());
                    if (!ok) unloadable_.insert(fi.absoluteFilePath());
                } else if (fi.isDir()) {
                    queue << fi.absoluteFilePath();
                }
            }
        }
        std::sort(found.begin(), found.end(),
                  [](const QPair<QString, QString> &a, const QPair<QString, QString> &b) {
                      return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
                  });
        for (const auto &f : found) {
            paths_ << f.second;
            pluginList_->addItem(f.first);
        }
        fprintf(stderr, "pestudio: scanned %s -> %d plugin(s)\n",
                qPrintable(root.absolutePath()), int(paths_.size())); fflush(stderr);
        statusBar()->showMessage(QString("%1 plugin(s) under %2").arg(paths_.size()).arg(root.absolutePath()));
        /* Load something straight away: an empty host makes an attached
         * keyboard look broken when it is only unassigned. */
        if (!paths_.isEmpty() && pluginList_->currentRow() < 0) {
            int want = -1;
            if (!startPlugin_.isEmpty()) {
                /* Named on the command line, so it wins over both the first
                 * entry and the crash guard -- asking for a plugin by name is
                 * explicit enough to mean "try it anyway". Consumed here so a
                 * later rescan browses normally rather than jumping back. */
                const QString abs = QFileInfo(startPlugin_).absoluteFilePath();
                const QString base = QFileInfo(startPlugin_).fileName();
                startPlugin_.clear();
                for (int i = 0; i < paths_.size(); i++)
                    if (paths_[i] == abs) { want = i; break; }
                if (want < 0) {
                    /* Applying the bank to whatever happened to load first
                     * would be worse than not loading: it would look like it
                     * worked. The list stays populated, so it can still be
                     * clicked once the right plugin is selected by hand. */
                    bankApplied_ = true;
                    statusBar()->showMessage(
                        base + " is not a plugin this host can load, or is not "
                               "under " + root.absolutePath(), 0);
                    info_->setText("<b>" + base.toHtmlEscaped() + "</b> was named on "
                                   "the command line but is not in the list.<br>"
                                   "<span style='color:#888'>Nothing was loaded.</span>");
                }
            } else {
                want = 0;
                while (want < paths_.size() && paths_[want] == crashed_) want++;
                if (want >= paths_.size()) want = -1;
            }
            if (want >= 0) pluginList_->setCurrentRow(want);
        }
    }

    static QString markerPath()
    {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDir().mkpath(dir);
        return dir + "/loading";
    }

    void loadRow(int row)
    {
        if (row < 0 || row >= paths_.size()) return;
        if (g_inPlugin) {
            /* Reached from inside a plugin call, by way of the input pump.
             * Closing the plugin now would free the interpreter that is running. */
            statusBar()->showMessage("finish the gesture before changing plugin", 2000);
            return;
        }
        QString err;

        /* Marked unsupported during the scan; say so rather than crashing into
         * a failed load. */
        if (unloadable_.contains(paths_[row])) {
            editor_->detach();
            pixelEditor_->detach();
            info_->setText("<b>" + QFileInfo(paths_[row]).fileName().toHtmlEscaped() +
                           "</b> cannot be loaded by this host.<br>"
                           "<span style='color:#888'>Only 64-bit x86 plugins are "
                           "supported.</span>");
            statusBar()->showMessage("unsupported plugin", 4000);
            return;
        }

        /* Everything sounding stops before the plugin under it is taken away,
         * and it stops by being *released* rather than by going quiet with the
         * plugin.
         *
         * A held note is a note-on that has gone three places: the plugin, the
         * internal source that feeds an effect, and out of the MIDI port. Only
         * the first of those dies with the plugin. The gate feeding an effect
         * would still be open for whatever loads next, and the hardware
         * listening on the port would hold the note for good -- a stuck note on
         * a synth in the rack, which no amount of clicking in here will clear.
         * Releasing first sends the note-offs while all three still mean
         * something. The all-notes-off behind it covers what this window does
         * not know is sounding: a sustain pedal, a thru'd channel, anything the
         * plugin latched itself. */
        if (piano_) piano_->releaseAll();
        if (eng_.host()) pehost_all_notes_off(eng_.host());
        eng_.allNotesOff();

        /* Note what we are about to load. A plugin that faults inside its own
         * DSP takes this process with it -- nothing can be caught in-process --
         * so if this marker is still here next start, that plugin is the
         * culprit and gets skipped instead of wedging startup forever. */
        {
            QFile f(markerPath());
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(paths_[row].toUtf8());
                f.close();
            }
        }
        programList_->clear();
        clearParams();
        /* Both editors reference the plugin that is about to be closed. */
        editor_->detach();
        pixelEditor_->detach();
        /* And the zoom belongs to the editor that is going with it. */
        editorW_ = editorH_ = 0;
        zoom_ = 1.0;
        updateZoomUi();
        /* A reading left over from an external wheel would otherwise claim the
         * plugin about to be opened is already bent. Shown, not sent: there is
         * no plugin to send it to yet, and a fresh one starts centred. */
        if (wheel_) wheel_->setBend(PitchWheel::kCentre);

        /* Say what is being loaded *before* loading it. A plugin that hangs or
         * dies takes the report with it, and then there is nothing to say which
         * one it was -- which is exactly when you need to know. */
        fprintf(stderr, "pestudio: loading %s ...\n",
                qPrintable(QFileInfo(paths_[row]).fileName()));
        fflush(stderr);
        QApplication::setOverrideCursor(Qt::WaitCursor);
        QElapsedTimer tOpen; tOpen.start();
        pehost_kind kind = forceBox_ ? (pehost_kind)forceBox_->currentData().toInt()
                                     : PEHOST_KIND_AUTO;
        bool ok = eng_.load(paths_[row], &err, kind);
        qint64 msOpen = tOpen.elapsed();
        QApplication::restoreOverrideCursor();
        if (!ok) {
            info_->setText("<b>Load failed:</b> " + err.toHtmlEscaped());
            statusBar()->showMessage("load failed: " + err);
            return;
        }
        pehost *h = eng_.host();
        loadedPath_ = paths_[row];
        /* An effect with nothing fed to it can only be silent, which reads as a
         * broken plugin. Start it on the keys; a synth is left on silence because
         * anything fed to it would only be added to what it generates. */
        if (srcBox_) {
            int want = pehost_is_synth(h) ? int(Engine::SrcSilence) : int(Engine::SrcNotes);
            int ix = srcBox_->findData(want);
            if (ix >= 0 && ix != srcBox_->currentIndex()) srcBox_->setCurrentIndex(ix);
            else eng_.setSource(Engine::Source(want));
        }
        int impl = 0, stub = 0, called = 0;
        pehost_import_stats(&impl, &stub, &called);
        info_->setText(QString(
            "<b>%1</b> &mdash; %2<br>"
            "%3 &nbsp; in %4 / out %5 &nbsp; programs %6 &nbsp; params %7<br>"
            "<span style='color:#888'>uniqueID 0x%8 &nbsp; imports: %9 native, "
            "%10 stubbed, %11 reached</span>")
            .arg(QString::fromLocal8Bit(pehost_name(h)).toHtmlEscaped())
            .arg(QString::fromLocal8Bit(pehost_vendor(h)).toHtmlEscaped())
            .arg(pehost_is_synth(h) ? "synth" : "effect")
            .arg(pehost_num_inputs(h)).arg(pehost_num_outputs(h))
            .arg(pehost_num_programs(h)).arg(pehost_num_params(h))
            .arg(uint(pehost_unique_id(h)), 8, 16, QChar('0'))
            .arg(impl).arg(stub).arg(called));

        for (int i = 0; i < pehost_num_programs(h); i++) {
            char nm[64];
            pehost_program_name(h, i, nm, sizeof nm);
            programList_->addItem(QString("%1  %2").arg(i, 3).arg(QString::fromLocal8Bit(nm)));
        }
        QElapsedTimer tUi; tUi.start();
        buildParams();
        qint64 msUi = tUi.elapsed();

        /* The editor is opened on demand rather than on selection. Browsing a
         * folder should not instantiate a GUI per plugin -- each one costs a
         * window, a GL context and whatever state the plugin keeps -- and a
         * misbehaving editor then only misbehaves when actually asked for. */
        editorKind_ = pehost_editor_kind(h);
        editorOpened_ = false;
        tabs_->setTabEnabled(1, editorKind_ != PEHOST_EDITOR_NONE);
        tabs_->setTabText(1, editorKind_ == PEHOST_EDITOR_NONE ? "Editor (n/a)" : "Editor");
        if (editorKind_ != PEHOST_EDITOR_NONE) {
            int w = 0, ht = 0;
            pehost_editor_size(h, &w, &ht);
            tabs_->setTabToolTip(1, QString("%1x%2%3").arg(w).arg(ht)
                                    .arg(pehost_editor_can_resize(h) ? ", resizable" : ""));
        }
        tabs_->setCurrentIndex(0);

        if (programList_->count()) programList_->setCurrentRow(0);
        /* The patches that belong to *this* plugin, and then one of them.
         * Strictly after the program selection above, which dispatches
         * effSetProgram and so overwrites every parameter a patch sets -- the
         * other order silently discards the whole thing. */
        rebuildPatchList();
        if (bank_ && patchList_->count()) {
            int want;
            if (!bankApplied_) {
                /* --pick names a patch in the whole bank; the list holds only
                 * this plugin's, so translate rather than index straight in. */
                want = patchRows_.indexOf(startPatch_);
                if (want < 0) want = 0;
            } else {
                /* Same row as before the switch. Every bank here carries the
                 * same sounds in the same order, so staying on row 2 means
                 * staying on "cancel" -- which is what makes stepping through
                 * synths a comparison rather than a reshuffle. */
                want = qBound(0, wantPatchRow_, patchList_->count() - 1);
            }
            bankApplied_ = true;
            patchList_->setCurrentRow(want);
        }
        piano_->setFocus();
        fprintf(stderr, "pestudio: load %s -- plugin %lld ms, parameter UI %lld ms (%d params)\n",
                qPrintable(QFileInfo(paths_[row]).fileName()), msOpen, msUi,
                pehost_num_params(h));
        fflush(stderr);
    }

    /* Instantiate the plugin's editor the first time its tab is shown. */
    void openEditor()
    {
        pehost *h = eng_.host();
        if (!h || editorOpened_ || editorKind_ == PEHOST_EDITOR_NONE) return;
        /* Instantiating an editor dispatches effEditOpen. The tab change that
         * gets us here can be delivered by the input pump, so this is reachable
         * from inside a plugin that is already running -- and --cycle switches
         * tabs on a timer, which is exactly such a delivery.
         *
         * editorOpened_ stays false so the next showing of the tab tries again,
         * and the refusal is reported: an editor silently skipped here would
         * look exactly like a plugin that has none. */
        if (g_inPlugin) {
            fprintf(stderr, "pestudio: editor open deferred -- inside a plugin call\n");
            fflush(stderr);
            return;
        }
        editorOpened_ = true;

        bool gui = false;
        int w = 0, ht = 0;
        pehost_editor_size(h, &w, &ht);
        if (editorKind_ == PEHOST_EDITOR_X11) {
            gui = editor_->attach(h);
            if (gui) editorStack_->setCurrentIndex(0);
        } else {
            pixelEditor_->dumped_ = false;
            pixelEditor_->dumpName_ = QFileInfo(paths_[pluginList_->currentRow()]).fileName();
            gui = pixelEditor_->attach(h);
            if (gui) editorStack_->setCurrentIndex(1);
        }
        if (gui && w > 0 && ht > 0) {
            editorW_ = w; editorH_ = ht;
            zoom_ = 1.0;
            applyZoom();
            /* Fit it if it does not already fit.
             *
             * Deferred, because the viewport has not been laid out at this
             * size yet and asking it now measures the last plug-in's editor.
             * Zooming out only: an editor smaller than the space is left at
             * the size the plug-in drew rather than blown up to fill the
             * window, which no plug-in's artwork survives. */
            QTimer::singleShot(0, this, [this] { zoomFit(true); });
        }
        updateZoomUi();
        if (!gui) {
            tabs_->setTabEnabled(1, false);
            tabs_->setTabText(1, "Editor (n/a)");
            tabs_->setCurrentIndex(0);
            statusBar()->showMessage("this plugin's editor could not be opened", 5000);
        }
    }

    /* ------------------------------------------------------------- zoom */

    /* What the buttons can do to the editor that is actually open.
     *
     * Two different mechanisms answer to them -- the pixel editor scales the
     * image it blits, a native editor is asked to lay itself out at another
     * size -- and one plug-in in the second group can refuse outright. Asked
     * rather than remembered, because it is the loaded plug-in that decides. */
    bool editorCanZoom() const
    {
        if (!editorOpened_ || editorW_ <= 0) return false;
        if (editorKind_ == PEHOST_EDITOR_PIXELS) return pixelEditor_->canZoom();
        if (editorKind_ == PEHOST_EDITOR_X11)    return editor_->canZoom();
        return false;
    }

    void setZoom(double z)
    {
        z = qBound(kZoomMin, z, kZoomMax);
        if (!editorCanZoom()) { updateZoomUi(); return; }
        zoom_ = z;
        applyZoom();
        updateZoomUi();
    }

    /* One notch. Geometric, not a fixed number of percent: stepping down from
     * 100 in tenths takes ten presses to halve the picture and then crawls,
     * where a constant ratio feels the same at every size. */
    void zoomStep(int dir)
    {
        if (dir) setZoom(zoom_ * (dir > 0 ? 1.25 : 1.0 / 1.25));
    }

    /* Scale the editor to the space there is.
     *
     * `onlyShrink` is what the automatic fit on opening an editor uses: an
     * editor that already fits is left at the size the plug-in drew it, because
     * enlarging one is not an improvement -- the artwork is bitmaps and text at
     * a fixed size, and stretching it just makes it soft. Pressing Fit is an
     * explicit request and will enlarge. */
    void zoomFit(bool onlyShrink = false)
    {
        if (!editorCanZoom() || editorW_ <= 0 || editorH_ <= 0) return;
        const QSize v = editorScroll_->viewport()->size();
        if (v.width() < 16 || v.height() < 16) return;   /* not laid out yet */
        const double z = qMin(double(v.width())  / editorW_,
                              double(v.height()) / editorH_);
        if (onlyShrink && z >= 1.0) { setZoom(1.0); return; }
        setZoom(z);
    }

    void applyZoom()
    {
        if (editorW_ <= 0 || editorH_ <= 0) return;
        bool scaled = false;
        if (editorKind_ == PEHOST_EDITOR_PIXELS)   scaled = pixelEditor_->setZoom(zoom_);
        else if (editorKind_ == PEHOST_EDITOR_X11) scaled = editor_->setZoom(zoom_);
        const double z = scaled ? zoom_ : 1.0;
        /* QStackedWidget sizes its pages to itself, so it has to be told how big
         * the editor is or the page gets squeezed to nothing. */
        const int sw = int(editorW_ * z + 0.5), sh = int(editorH_ * z + 0.5);
        editorStack_->setMinimumSize(sw, sh);
        editorStack_->resize(sw, sh);
    }

    void updateZoomUi()
    {
        const bool on = editorCanZoom();
        zoomOut_->setEnabled(on && zoom_ > kZoomMin);
        zoomIn_->setEnabled(on && zoom_ < kZoomMax);
        zoomFit_->setEnabled(on);
        zoom1to1_->setEnabled(on && zoom_ != 1.0);
        zoomLabel_->setEnabled(on);
        zoomLabel_->setText(QString("%1%").arg(int(zoom_ * 100.0 + 0.5)));
        /* Say why, when they are dead. A disabled button with no reason next to
         * it reads as something broken rather than something that cannot be
         * done to this particular plug-in. */
        if (on)
            zoomNote_->setText(editorKind_ == PEHOST_EDITOR_X11
                ? QString("the plug-in redraws itself at this size") : QString());
        else if (!editorOpened_ || editorW_ <= 0)
            zoomNote_->setText("no editor open");
        else if (editorKind_ == PEHOST_EDITOR_X11)
            zoomNote_->setText("this plug-in draws its own window and will not resize it");
        else
            zoomNote_->setText(QString());
    }

    void updateMidiSources()
    {
        if (!midi_ || !midi_->isOpen()) return;
        QStringList in = midi_->sources(), out = midi_->sinks();
        QString t;
        t += in.isEmpty() ? QString("in: nothing connected")
                          : QString("in: %1").arg(in.join(", "));
        if (!out.isEmpty()) t += QString("\nout: %1").arg(out.join(", "));
        midiSources_->setText(t);
    }

    void pollUi()
    {
        level_->setValue(int(eng_.peak() * 100.0f));
        /* Alongside the meter and before the g_inPlugin guard: a take running
         * while a plugin's editor is being dragged still has to show its clock. */
        tickRecord();
        /* refreshVisible() repaints parameter rows, and painting one asks the
         * plugin for its value and its display string. Reached from inside a
         * plugin's own drag loop by way of the input pump, that is a second
         * entry into a plugin that is already running. The level meter above
         * touches nothing of the plugin's, so it still updates. */
        if (g_inPlugin) return;
        refreshVisible();

        /* Follow the transport rather than assume it. Once a sequencer's clock is
         * driving the tempo, the box shows what it is doing instead of what
         * somebody typed earlier -- two numbers that disagree about the tempo is
         * worse than one that is merely read-only. */
        if (eng_.host()) {
            double bpm = pehost_tempo(eng_.host());
            bool rolling = pehost_playing(eng_.host()) != 0;
            if (fabs(bpm - tempoBox_->value()) > 0.05) {
                tempoFromClock_ = true;
                tempoBox_->setValue(bpm);
                tempoFromClock_ = false;
            }
            const char *state = clockSeen_ ? (rolling ? "following clock"
                                                      : "clock, stopped")
                                           : (rolling ? "internal" : "stopped");
            if (tempoSync_->text() != QLatin1String(state)) tempoSync_->setText(state);
        }
    }

private:
    /* Every plugin tree we can find, walking up from the binary. */
    static QList<QPair<QString, QString>> discoverRoots()
    {
        static const struct { const char *label, *rel; } cand[] = {
            { "Windows VST2 64-bit", "windows/VST2-64" },
            { "Windows VST3",        "windows/VST3"    },
            { "Linux native",        "linux/extracted" },
            { "Windows VST2 32-bit", "windows/VST2-32" },
#if PESTUDIO_MAC
            { "macOS VST2",          "macos/VST2"      },
            { "macOS Audio Units",   "macos/AU"        },
            { "Mac OS 9 (Classic)",  "macos/classic"   },
#endif
        };
        QList<QPair<QString, QString>> out;
        QDir d(QCoreApplication::applicationDirPath());
        for (int up = 0; up < 6; up++) {
            for (const auto &c : cand) {
                QString p = d.absoluteFilePath(c.rel);
                if (QDir(p).exists()) {
                    bool dup = false;
                    for (const auto &o : out) if (o.second == p) dup = true;
                    if (!dup) out << qMakePair(QString(c.label), p);
                }
            }
            if (!d.cdUp()) break;
        }
        // The system's own VST directories, so an installed copy has somewhere
        // to switch between rather than a selector with nothing in it.
        for (const QString &p : standardPluginDirs()) {
            bool dup = false;
            for (const auto &o : out) if (o.second == p) dup = true;
            if (!dup) out << qMakePair(QString(p.contains("vst3") ? "VST3" : "VST2"), p);
        }
        // Never empty: the selector names the directory being browsed, and
        // defaultDir() falls back to the home folder, so it has to be able to
        // say so rather than showing a blank.
        if (out.isEmpty())
            out << qMakePair(QString("Home folder"), QDir::homePath());
        return out;
    }

    // Where a Linux system keeps plug-ins, for an installed copy rather than a
    // checkout. The walk-up below is right in a tree and finds nothing once
    // this lives in /usr/lib/vst-ace, where it reaches /windows/VST2-64 -- a
    // path no machine has -- and the window opened on the home directory with
    // no explanation. Only directories that exist are returned.
    static QStringList standardPluginDirs()
    {
        QStringList out;
        const QString home = QDir::homePath();
        const QStringList cand = {
            home + "/.vst", home + "/.vst3",
            "/usr/lib/vst", "/usr/lib/vst3",
            "/usr/local/lib/vst", "/usr/local/lib/vst3",
            "/usr/lib/x86_64-linux-gnu/vst", "/usr/lib/x86_64-linux-gnu/vst3",
        };
        for (const QString &c : cand)
            if (QDir(c).exists() && !out.contains(c)) out << c;
        // VST_PATH and VST3_PATH are colon-separated, like PATH.
        for (const char *var : { "VST_PATH", "VST3_PATH" }) {
            const QString e = qEnvironmentVariable(var);
            if (e.isEmpty()) continue;
            for (const QString &part : e.split(':', Qt::SkipEmptyParts))
                if (QDir(part).exists() && !out.contains(part)) out << part;
        }
        return out;
    }

    static QString defaultDir()
    {
        // <repo>/re/peload/qtgui -> ../../../windows/VST2-64
        QDir d(QCoreApplication::applicationDirPath());
        for (int up = 0; up < 5; up++) {
            QString c = d.absoluteFilePath("windows/VST2-64");
            if (QDir(c).exists()) return c;
            if (!d.cdUp()) break;
        }
        const QStringList std = standardPluginDirs();
        if (!std.isEmpty()) return std.first();
        return QDir::homePath();
    }

    /* Program changes rewrite every parameter at once; the model only asks the
     * plugin about rows on screen, so a repaint is all that is needed. */
    /* True when the keyboard belongs to something that wants letters rather
     * than notes: a modal dialog, a popup, or any text-entry widget. QLineEdit
     * alone does not cover it -- a spin box holds its editor inside itself, and
     * an editable combo box likewise. */
    bool isTyping() const
    {
        if (qApp->activeModalWidget()) return true;
        /* A popup owns the keyboard for as long as it is up: a combo box list,
         * a menu, a completer. This is the case the blanket item-view test
         * below used to be standing in for, and it is the only one of them that
         * was ever real. */
        if (qApp->activePopupWidget()) return true;
        QWidget *f = qApp->focusWidget();
        if (!f) return false;
        if (qobject_cast<QLineEdit *>(f) || qobject_cast<QAbstractSpinBox *>(f) ||
            qobject_cast<QTextEdit *>(f) || qobject_cast<QPlainTextEdit *>(f))
            return true;
        if (auto *cb = qobject_cast<QComboBox *>(f)) return cb->isEditable();
        /* No test for an item view, on purpose.
         *
         * Every list in this window is one -- the plug-ins, the programs, the
         * patches, the parameter table -- and clicking one of them is how you
         * get anywhere at all. Treating a focused list as typing silenced the
         * computer keyboard for the rest of the session: zxcvb did nothing
         * until the on-screen keyboard was clicked to move focus back, which is
         * the whole of "the keys stop working after loading a plug-in". The
         * load path hides it on the way through -- loadRow ends with
         * piano_->setFocus() -- so it showed up after a load that failed or was
         * refused, and after every click on a program, a patch or a parameter.
         *
         * A view that really is being typed into is still caught: editing a
         * cell puts focus on the delegate's editor -- a QLineEdit -- and not on
         * the view, so the tests above have it. */
        /* Inside a dialog of any kind, modal or not. */
        for (QWidget *w = f; w; w = w->parentWidget())
            if (qobject_cast<QDialog *>(w)) return true;
        return false;
    }

    bool eventFilter(QObject *o, QEvent *ev) override
    {
        switch (ev->type()) {
        case QEvent::KeyPress:
        case QEvent::KeyRelease: {
            auto *k = static_cast<QKeyEvent *>(ev);
            const bool down = ev->type() == QEvent::KeyPress;
            QWidget *f = qApp->focusWidget();

            /* Typing somewhere that wants letters must not play the synth.
             *
             * The press path below already steps aside for a QLineEdit, but
             * that is not enough on its own: releases are deliberately always
             * delivered, so a filename typed into the save dialog sent a
             * note-off per keystroke, and a spin box or a completer popup is
             * not a QLineEdit at all. A modal dialog is the clearest signal
             * there is -- while one is up the keyboard belongs to it, both
             * directions -- and any text-entry widget is treated the same way.
             *
             * Held notes are given up on the way in. From inside a modal dialog
             * no key-up will ever reach the piano, which is exactly the case
             * that makes a note stick for good. */
            if (isTyping()) {
                if (piano_ && pianoWasLive_) { piano_->releaseAll(); pianoWasLive_ = false; }
                break;
            }
            pianoWasLive_ = true;
            /* Presses and releases are treated differently on purpose.
             *
             * A press only starts a note when the letters are not meant for
             * something else. Typing into one of our own fields is easy to spot;
             * a text field *inside* a plugin's editor is not -- Cardinal has a
             * whole text-editor module -- so an editor with focus is left alone
             * entirely. Otherwise every letter typed in there would also play a
             * note.
             *
             * A release is always delivered, wherever focus has gone. That is the
             * point: a note is held precisely so the hand is free to reach into
             * the editor, and the key-up then arrives over the editor. Ignoring
             * it there is what made notes stick. */
            const bool inEditor = f && (f == pixelEditor_ || f == editor_ ||
                                        (editorStack_ && editorStack_->isAncestorOf(f)));
            if (down) {
                if (qobject_cast<QLineEdit *>(f)) break;
                if (inEditor) break;
                /* A press carrying Ctrl, Alt or Meta is a command, whether or
                 * not anything here claims it. Qt eats the combinations that
                 * are real shortcuts before this, so what arrives is the ones
                 * that are not -- and Ctrl+C, Ctrl+V, Ctrl+X and Ctrl+B all
                 * land on note keys. Playing a note at the copy shortcut is not
                 * something to do in front of an audience.
                 *
                 * Presses only. A release is delivered whatever is held with
                 * it, because reaching for a modifier while a note is down must
                 * not be what strands that note on. */
                if (k->modifiers() & (Qt::ControlModifier | Qt::AltModifier |
                                      Qt::MetaModifier))
                    break;
            }
            /* A key that played a note is eaten, so it cannot also do something
             * else on the way past.
             *
             * An item view answers a plain letter with keyboardSearch(), which
             * moves the current row -- and on the plug-in list that means
             * loading whatever plug-in the letter lands on. Playing zxcvb with
             * the list focused would otherwise walk through the corpus loading
             * synths, which is worse than the silence it replaced.
             *
             * Not over the editor: a key-up is deliberately still delivered
             * there so a note held while reaching into the plug-in's GUI does
             * not stick, and the plug-in is entitled to see it too. */
            if (piano_ && piano_->routeKey(k->key(), down, k->isAutoRepeat()) &&
                !inEditor)
                return true;
            break;
        }
        case QEvent::WindowDeactivate:
            /* The desktop took focus away: from here no key-up will ever arrive,
             * so anything still down would stick for good. */
            if (piano_) piano_->releaseAll();
            break;
        default:
            break;
        }
        return QMainWindow::eventFilter(o, ev);
    }

    /* Apply one patch from the loaded bank. Reached from the Patches list, so
     * clicking a name is the whole interaction -- which is the point of a bank
     * over a single --patch file.
     *
     * A bank stays usable after the plugin is switched rather than being
     * unloaded with it: patches are matched by parameter name, and applying one
     * to a sibling synth is a reasonable thing to try. patch_bank_apply says so
     * when the uniqueID disagrees, which is the honest middle ground between
     * refusing and doing it silently. */
    void applyPatchRow(int row)
    {
        if (!bank_ || row < 0 || row >= patchRows_.size()) return;
        if (g_inPlugin) {
            statusBar()->showMessage("finish the gesture before changing patch", 2000);
            return;
        }
        patch_bank *b  = activeBank();
        const int   ix = patchRows_[row];
        wantPatchRow_  = row;
        if (ix < 0) {                       /* the "none" row: stand down */
            statusBar()->showMessage("no patch -- the Programs list is live again",
                                     5000);
            return;
        }
        /* A patch may name its own plugin, which is what lets one file span
         * machines: selecting it means load that synth, then apply this. The
         * guard matters because loadRow can select a patch row in turn, and two
         * of these interleaving would load a plugin while one was mid-load. */
        if (!switching_) {
            /* Canonical on both sides. A bank's path is resolved against the
             * bank file and arrives full of ".." while loadedPath_ is already
             * absolute, so comparing the strings as written says "different"
             * for the plugin that is open -- and the startup patch reloaded the
             * synth it had just finished loading. */
            const QString want = QFileInfo(QString::fromLocal8Bit(
                patch_bank_patch_plugin_path(b, ix))).canonicalFilePath();
            if (!want.isEmpty() &&
                want != QFileInfo(loadedPath_).canonicalFilePath()) {
                switching_ = true;
                bool ok = loadPluginPath(want);
                switching_ = false;
                if (!ok) {
                    statusBar()->showMessage(
                        "patch wants " + QFileInfo(want).fileName() +
                        ", which could not be loaded", 0);
                    return;
                }
            }
        }
        if (!eng_.host()) return;
        char err[256];
        int  applied = 0, missed = 0;
        if (patch_bank_apply(b, ix, eng_.host(), err, sizeof err,
                             &applied, &missed) != 0) {
            statusBar()->showMessage("patch: " + QString::fromLocal8Bit(err), 0);
            fprintf(stderr, "pestudio: patch failed -- %s\n", err);
            fflush(stderr);
            return;
        }
        /* The program the patch asked for, reflected in the list without
         * dispatching effSetProgram a second time -- which would overwrite every
         * parameter the patch just set. setCurrentRow fires the handler that
         * does exactly that, so the signal is blocked and the selection follows
         * rather than driving. */
        if (int prog = pehost_get_program(eng_.host());
            prog >= 0 && prog < programList_->count()) {
            QSignalBlocker block(programList_);
            programList_->setCurrentRow(prog);
        }
        refreshParams();

        QString msg = QString("%1: %2 parameter(s) set")
                          .arg(QString::fromLocal8Bit(patch_bank_patch_name(b, ix)))
                          .arg(applied);
        if (missed) msg += QString(", %1 matched nothing").arg(missed);
        if (err[0]) msg += " -- " + QString::fromLocal8Bit(err);
        statusBar()->showMessage(msg, err[0] ? 0 : 6000);
        fprintf(stderr, "pestudio: %s\n", qPrintable(msg));
        fflush(stderr);
    }

    patch_bank *activeBank() const { return usingAuto_ ? autoBank_ : bank_; }

    /* Put the selected patch back on top of whatever program was just chosen.
     *
     * Without this, selecting a patch and then a program gave two different
     * sounds for the same patch -- the program having overwritten every
     * parameter behind it. A patch carries the plugin's whole parameter set, so
     * re-applying it restores its sound exactly; only the program's own
     * selection is left alone, so the Programs list still shows where the user
     * put it. */
    void reassertPatch()
    {
        const int row = patchList_ ? patchList_->currentRow() : -1;
        if (!bank_ || row < 1 || row >= patchRows_.size()) return;
        if (patchRows_[row] < 0) return;
        char err[256];
        int  applied = 0, missed = 0;
        if (patch_bank_apply_params(activeBank(), patchRows_[row], eng_.host(),
                                    err, sizeof err, &applied, &missed) == 0)
            statusBar()->showMessage(
                QString("program changed; \"%1\" re-applied over it "
                        "(%2 parameters) -- patches override programs")
                    .arg(patchList_->item(row)->text()).arg(applied), 5000);
    }

    /* Rebuild the Patches list for whatever plugin is now loaded.
     *
     * A bank spanning machines holds patches for all of them, so only the ones
     * belonging to the plugin on screen should be offered -- otherwise selecting
     * one silently reloads a different synth, and the list stops describing what
     * you are looking at. A patch naming no plugin belongs to whatever is open,
     * which is what keeps an ordinary single-plugin bank working unchanged.
     *
     * When the opened bank has nothing for this plugin, one is looked for on
     * disk beside it, so that switching to any synth in the browser still brings
     * up that synth's patches rather than an empty list or -- worse -- another
     * machine's. */
    void rebuildPatchList()
    {
        if (!bank_) return;
        const QString cur = QFileInfo(loadedPath_).canonicalFilePath();

        usingAuto_ = false;
        patchRows_ = rowsFor(bank_, cur);
        if (patchRows_.isEmpty() && loadPluginBank(cur)) {
            usingAuto_ = true;
            patchRows_ = rowsFor(autoBank_, cur);
        }

        /* Row 0 is not a patch. With a patch selected the Programs list stops
         * being audible -- the patch is re-asserted over every program change,
         * which is the point -- so there has to be a way to stand down and hear
         * the plugin's own presets again. */
        patchRows_.prepend(-1);

        QSignalBlocker block(patchList_);   /* filling is not a selection */
        patchList_->clear();
        patch_bank *b = activeBank();
        for (int ix : patchRows_)
            patchList_->addItem(ix < 0
                ? QString("— none (use the Programs list) —")
                : QString::fromLocal8Bit(patch_bank_patch_name(b, ix)));

        const QString src = usingAuto_ ? autoBankFile_ : bankFile_;
        patchLabel_->setText(patchRows_.isEmpty()
            ? QString("Patches -- none for this plugin")
            : QString("Patches -- %1").arg(QFileInfo(src).fileName()));
        patchLabel_->setToolTip(src);
    }

    static QVector<int> rowsFor(patch_bank *b, const QString &plugin)
    {
        QVector<int> rows;
        for (int i = 0; i < patch_bank_count(b); i++) {
            const char *pp = patch_bank_patch_plugin_path(b, i);
            const QString p = *pp
                ? QFileInfo(QString::fromLocal8Bit(pp)).canonicalFilePath()
                : QString();
            if (p.isEmpty() || p == plugin) rows << i;
        }
        return rows;
    }

    /* Look for a bank belonging to `plugin`, beside the one that was opened:
     * either <stem>-menu.json next to it or under a menu/ directory there --
     * which is where make_menu_banks.py puts them. */
    bool loadPluginBank(const QString &plugin)
    {
        if (plugin.isEmpty()) return false;
        const QString stem = QFileInfo(plugin).completeBaseName();
        const QDir dir(QFileInfo(bankFile_).absolutePath());
        /* Hand-tuned first, then generated. A machine that has been done by
         * hand should never be answered with the generated version of itself,
         * and the order is the only thing that decides it. */
        for (const QString &cand : { dir.filePath("tuned/" + stem + "-menu.json"),
                                     dir.filePath(stem + "-menu.json"),
                                     dir.filePath("menu/" + stem + "-menu.json") }) {
            if (!QFileInfo::exists(cand) || cand == QFileInfo(bankFile_).absoluteFilePath())
                continue;
            char err[256];
            patch_bank *b = patch_bank_read(cand.toLocal8Bit().constData(),
                                            err, sizeof err);
            if (!b) {
                fprintf(stderr, "pestudio: %s\n", err);
                continue;
            }
            patch_bank_free(autoBank_);
            autoBank_ = b;
            autoBankFile_ = cand;
            /* Say where the patches came from. Without this a patch list that
             * quietly fell back to a generated bank is indistinguishable from
             * the one that was opened, and the only clue is a missing suffix in
             * the log. */
            /* With the directory, not just the name: tuned/kern64-menu.json and
             * menu/kern64-menu.json are different files with the same name, and
             * which one answered is the whole point of saying anything. */
            const QFileInfo fi(cand);
            fprintf(stderr, "pestudio: %s has no patches in %s -- using %s/%s\n",
                    qPrintable(QFileInfo(plugin).fileName()),
                    qPrintable(QFileInfo(bankFile_).fileName()),
                    qPrintable(fi.dir().dirName()), qPrintable(fi.fileName()));
            fflush(stderr);
            return true;
        }
        return false;
    }

    /* Load a plugin by path, wherever it lives. A cross-machine bank names
     * plugins that need not be under the directory being browsed, so one that
     * is not already in the list is appended to it -- both so loadRow can do
     * the actual work, and so the plugin is visibly there afterwards rather
     * than having been loaded by an invisible side door. */
    /* Auto-detect a file for the status line: name, the platform/loader it needs,
     * and why it cannot be loaded when that is the case. */
    static QString describeFile(const QString &path)
    {
        pehost_info info;
        pehost_classify(path.toLocal8Bit().constData(), &info);
        QString s = QFileInfo(path).fileName() + "  --  " + QString::fromUtf8(info.label);
        if (!info.loadable && info.why[0])
            s += "  (can't load: " + QString::fromUtf8(info.why) + ")";
        return s;
    }

    /* The user's own scan folders, persisted so a Downloads or system-VST
     * directory added once is still there next launch. The search set becomes
     * the user's, not only the built-in list in discoverRoots(). */
    void saveUserRoots()
    {
        QSettings s("pestudio", "pestudio");
        s.setValue("userRoots", userRoots_);
    }
    void loadUserRoots()
    {
        QSettings s("pestudio", "pestudio");
        const QStringList saved = s.value("userRoots").toStringList();
        for (const QString &d : saved) addUserRoot(d, /*select=*/false);
    }
    /* Add a folder as a browsing root. Deduped against what is already listed;
     * a genuinely new one is remembered. `select` switches the browser to it,
     * which triggers a rescan so the folder's plugins appear at once. */
    void addUserRoot(const QString &dir, bool select)
    {
        const QString abs = QDir(dir).absolutePath();
        for (int i = 0; i < rootBox_->count(); i++)
            if (rootBox_->itemData(i).toString() == abs) {
                if (select) rootBox_->setCurrentIndex(i);
                return;
            }
        QString name = QFileInfo(abs).fileName();
        rootBox_->addItem((name.isEmpty() ? abs : name) + "  [added]", abs);
        if (!userRoots_.contains(abs)) { userRoots_ << abs; saveUserRoots(); }
        if (select) rootBox_->setCurrentIndex(rootBox_->count() - 1);
    }

    /* File: open one plug-in, or add a folder of them.
     *
     * Both used to be buttons under the plug-in list. A menu is where a user
     * looks for "open", it keeps the keyboard shortcut somewhere discoverable,
     * and dwstudio carries the same two commands under the same name -- so
     * whichever window is open, the way in is the same. */
    void buildMenus()
    {
        QMenu *file = menuBar()->addMenu("&File");

        QAction *openVst = file->addAction("&Open VST...");
        openVst->setShortcut(QKeySequence::Open);              /* Ctrl+O */
        connect(openVst, &QAction::triggered, this, [this] {
            QString f = QFileDialog::getOpenFileName(
                this, "Open VST", dirEdit_->text(),
                "Plug-ins (*.dll *.so *.vst3 *.vst *.component);;All files (*)");
            if (f.isEmpty()) return;
            statusBar()->showMessage(describeFile(f));
            if (!loadPluginPath(f))
                statusBar()->showMessage("load failed: " +
                                         QString::fromUtf8(pehost_last_error()));
        });

        QAction *loadFolder = file->addAction("&Load Folder...");
        loadFolder->setShortcut(QKeySequence("Ctrl+L"));
        connect(loadFolder, &QAction::triggered, this, [this] {
            QString dir = QFileDialog::getExistingDirectory(this, "Load plug-in folder",
                                                            dirEdit_->text());
            if (!dir.isEmpty()) addUserRoot(dir, /*select=*/true);
        });

        file->addSeparator();
        QAction *quit = file->addAction("&Quit");
        quit->setShortcut(QKeySequence::Quit);
        connect(quit, &QAction::triggered, this, &QWidget::close);

        /* Which build this is. Worth having in the window rather than only on
         * the command line: the usual way this gets asked is somebody
         * reporting behaviour from a copy neither of us can identify. */
        QMenu *help = menuBar()->addMenu("&About");
        QAction *about = help->addAction("&About vst-ace");
        /* A plain QDialog rather than QMessageBox::about().
         *
         * The convenience function asks the platform theme for a native
         * message dialog, and on a desktop whose theme advertises one and then
         * hands back nothing, Qt calls through the null helper while tearing
         * the dialog down: QDialogPrivate::setNativeDialogVisible jumps to
         * address zero and the whole host dies, taking any loaded plug-in with
         * it. An About box is a label and a button; it is not worth reaching
         * through a platform helper to draw one, and building it here cannot
         * take that path at all. */
        connect(about, &QAction::triggered, this, [this] {
            QDialog dlg(this);
            dlg.setWindowTitle("About vst-ace");
            dlg.setModal(true);

            QString git = QString(VSTACE_GIT).isEmpty()
                              ? QString()
                              : QString("<br>Commit %1").arg(VSTACE_GIT);
            QLabel *body = new QLabel(
                QString("<b>vst-ace %1</b><br><br>"
                        "Built %2%3<br><br>"
                        "Runs Windows, macOS and Linux audio plug-ins as native "
                        "code on Linux &mdash; a PE loader with a Win32 subsystem "
                        "under it, a Mach-O loader with an Objective-C runtime, "
                        "and a CFM/PEF interpreter. Not emulation, and not Wine."
                        "<br><br>"
                        "This window is pestudio (Qt %4).")
                    .arg(VSTACE_VERSION).arg(VSTACE_BUILD_DATE)
                    .arg(git).arg(QT_VERSION_STR),
                &dlg);
            body->setTextFormat(Qt::RichText);
            body->setWordWrap(true);
            body->setMinimumWidth(420);

            QPushButton *close = new QPushButton("Close", &dlg);
            close->setDefault(true);
            connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);

            QVBoxLayout *lay = new QVBoxLayout(&dlg);
            lay->addWidget(body);
            QHBoxLayout *row = new QHBoxLayout;
            row->addStretch();
            row->addWidget(close);
            lay->addLayout(row);

            dlg.exec();
        });
    }

    bool loadPluginPath(const QString &path)
    {
        const QString abs = QFileInfo(path).absoluteFilePath();
        int ix = paths_.indexOf(abs);
        if (ix < 0) {
            if (!QFileInfo::exists(abs)) return false;
            paths_ << abs;
            pluginList_->addItem(QFileInfo(abs).fileName());
            ix = paths_.size() - 1;
        }
        {   /* Move the selection without the signal, or loadRow runs twice. */
            QSignalBlocker block(pluginList_);
            pluginList_->setCurrentRow(ix);
        }
        loadRow(ix);
        return eng_.host() != nullptr;
    }

    /* Start or stop a take. The file is named for the plugin and the moment, so
     * a session leaves a directory you can read rather than take-1, take-2. */
    void toggleRecord()
    {
        if (eng_.rec_.active()) {
            const uint64_t dropped = eng_.rec_.dropped();
            const QString file = eng_.rec_.stop();
            const double secs = double(eng_.rec_.frames()) / kSampleRate;
            recBtn_->setText("● Record");
            recBtn_->setStyleSheet("");
            recLabel_->setText("");
            /* Ask where it should live, starting from where the last one went.
             * The take is already on disk and playable by this point, so
             * cancelling is not losing anything -- it just stays in renders/.
             * Asking *after* rather than before is deliberate: a dialog between
             * pressing record and playing would cost the first bar. */
            QString target = QFileDialog::getSaveFileName(
                this, "Save recording as",
                QDir(saveDir_.isEmpty() ? QFileInfo(file).absolutePath() : saveDir_)
                    .filePath(QFileInfo(file).fileName()),
                "WAV audio (*.wav);;All files (*)");
            QString finalPath = file;
            if (!target.isEmpty() &&
                QFileInfo(target).absoluteFilePath() != QFileInfo(file).absoluteFilePath()) {
                if (!target.endsWith(".wav", Qt::CaseInsensitive)) target += ".wav";
                QFile::remove(target);              /* the dialog already asked */
                /* rename() will not cross a filesystem, and renders/ and a home
                 * directory are often on different ones here, so fall back. */
                if (QFile::rename(file, target) ||
                    (QFile::copy(file, target) && QFile::remove(file))) {
                    finalPath = target;
                    saveDir_ = QFileInfo(target).absolutePath();
                } else {
                    statusBar()->showMessage("could not write " + target +
                                             " -- left it in " + file, 0);
                }
            }
            QString msg = QString("wrote %1 -- %2.%3 s")
                              .arg(finalPath)
                              .arg(int(secs)).arg(int(secs * 10) % 10);
            if (dropped)
                msg += QString("  (%1 frames dropped -- the disk could not keep up)")
                           .arg(dropped);
            statusBar()->showMessage(msg, 0);
            fprintf(stderr, "pestudio: %s\n", qPrintable(msg));
            fflush(stderr);
            return;
        }
        if (!eng_.audioRunning()) {
            statusBar()->showMessage("no audio stream -- nothing to record", 4000);
            return;
        }
        QDir().mkpath("renders");
        const QString who = eng_.host() ? QString::fromLocal8Bit(pehost_name(eng_.host()))
                                        : QString("session");
        const QString safe = QString(who).replace(QRegularExpression("[^A-Za-z0-9._-]"), "-");
        const QString file = QString("renders/%1-%2.wav")
            .arg(safe, QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
        if (!eng_.rec_.start(file, kSampleRate)) {
            statusBar()->showMessage("could not open " + file + " for writing", 0);
            return;
        }
        recBtn_->setText("■ Stop");
        recBtn_->setStyleSheet("QPushButton { color: #d04040; font-weight: bold; }");
        statusBar()->showMessage("recording to " + file, 0);
        fprintf(stderr, "pestudio: recording to %s\n", qPrintable(file));
        fflush(stderr);
    }

    /* Elapsed time while a take runs, from the frames actually written -- not a
     * wall clock, so it shows what is in the file rather than how long the
     * button has been down. */
    void tickRecord()
    {
        if (!eng_.rec_.active()) return;
        const double secs = double(eng_.rec_.frames()) / kSampleRate;
        recLabel_->setText(QString("recording  %1:%2.%3")
            .arg(int(secs) / 60, 2, 10, QChar('0'))
            .arg(int(secs) % 60, 2, 10, QChar('0'))
            .arg(int(secs * 10) % 10));
    }

    void refreshParams() { refreshVisible(); }

    void clearParams() { paramModel_->setHost(nullptr); }

    void buildParams()
    {
        paramModel_->setHost(eng_.host());
        paramTable_->setColumnWidth(ParamModel::ColName, 190);
        paramTable_->setColumnWidth(ParamModel::ColValue,
                                    qMax(160, paramTable_->viewport()->width() - 300));
        paramTable_->setColumnWidth(ParamModel::ColDisplay, 100);
    }

    /* Which rows are actually visible, so polling costs the same whether the
     * plugin has 12 parameters or 2855. */
    void refreshVisible()
    {
        if (!eng_.host() || paramModel_->rowCount() == 0) return;
        const QModelIndex top = paramTable_->indexAt(QPoint(2, 2));
        const QModelIndex bot = paramTable_->indexAt(
            QPoint(2, paramTable_->viewport()->height() - 2));
        int first = top.isValid() ? top.row() : 0;
        int last  = bot.isValid() ? bot.row() : first + 40;
        paramModel_->refresh(first, last);
    }

    QString       startPlugin_;      /* consumed on first use -- see rescan() */
    QString       bankFile_, autoBankFile_;
    /* bank_ is what was opened and is never replaced. autoBank_ is one found on
     * disk for a plugin bank_ says nothing about, so that selecting any synth
     * still brings up its patches; it is replaced on each such switch. Whichever
     * supplies the current list is activeBank(). */
    patch_bank   *bank_ = nullptr;      /* owned; freed in ~Window */
    patch_bank   *autoBank_ = nullptr;  /* owned; freed on replace and in ~Window */
    bool          usingAuto_ = false;
    QVector<int>  patchRows_;           /* list row -> patch index in activeBank() */
    int           wantPatchRow_ = 0;    /* filtered row kept across plugin switches */
    int           startPatch_ = 0;      /* which patch --pick chose */
    bool          bankApplied_ = false;
    bool          switching_ = false;   /* inside a patch-driven plugin change */
    QString       loadedPath_;          /* what is open, for cross-machine banks */
    Engine        eng_;
    QLineEdit    *dirEdit_;
    QComboBox    *rootBox_;
    QComboBox    *forceBox_ = nullptr;   /* "Load as": force a loader, or auto-detect */
    QStringList   userRoots_;            /* extra scan folders the user added, persisted */
    QComboBox    *srcBox_;
    QListWidget  *pluginList_, *programList_, *patchList_;
    QPushButton  *recBtn_ = nullptr;
    QString       saveDir_;      /* where the last take was saved */
    bool          pianoWasLive_ = true;  /* had focus before typing began */
    QLabel       *recLabel_ = nullptr;
    QLabel       *patchLabel_;
    QDoubleSpinBox *tempoBox_ = nullptr;
    QLabel         *tempoSync_ = nullptr;
    bool            tempoFromClock_ = false;
    bool            clockSeen_ = false;
    QLabel       *info_;
    ParamModel   *paramModel_;
    ParamTable   *paramTable_;
    QTabWidget   *tabs_;
    EditorHost   *editor_ = nullptr;
    PixelEditor  *pixelEditor_ = nullptr;
    QStackedWidget *editorStack_;
    QScrollArea  *editorScroll_;
    QSlider      *gain_;
    QProgressBar *level_;
    Piano        *piano_;
    PitchWheel   *wheel_ = nullptr;
    MidiIo       *midi_ = nullptr;
    QLabel       *midiPort_, *midiSources_;
    QComboBox    *midiChan_;
    QCheckBox    *midiThru_, *midiOutAuto_;
    QStringList   paths_;
    QSet<QString> unloadable_;
    QString       crashed_;
    QTimer       *cycleTimer_ = nullptr;
    int           editorKind_ = 0;
    bool          editorOpened_ = false;
    /* The size the plug-in drew its editor at, and what the zoom is a multiple
     * of. Not the widget's size, which is the two multiplied together. */
    int           editorW_ = 0, editorH_ = 0;
    double        zoom_ = 1.0;
    QToolButton  *zoomOut_ = nullptr, *zoomIn_ = nullptr;
    QPushButton  *zoomFit_ = nullptr, *zoom1to1_ = nullptr;
    QLabel       *zoomLabel_ = nullptr, *zoomNote_ = nullptr;

};

#include "main.moc"

/* Let the window system deliver input while a plugin spins in its own drag loop.
 * Bounded, because this is called from inside that loop. */
static void pump_input(void *ud)
{
    (void)ud;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
}

int main(int argc, char **argv)
{
    /* Plugin editors are X11 windows, so this process has to be an X11 client.
     *
     * A VST3 editor is attached by handing the plugin a window id it treats as
     * an X11 Window (kPlatformTypeX11EmbedWindowID); a VST2 editor on Linux is
     * the same idea. Under Qt's Wayland backend, winId() is a Wayland surface
     * handle instead -- so the plugin runs Xlib calls against a window that does
     * not exist and dies inside its own GL setup, which looks for all the world
     * like a crash on load. XWayland is present on every Wayland desktop that
     * can run these plugins at all, so asking for xcb costs nothing and makes
     * embedding work. Set QT_QPA_PLATFORM yourself to override. */
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        const QByteArray session = qgetenv("XDG_SESSION_TYPE");
        if (session == "wayland" || qEnvironmentVariableIsSet("WAYLAND_DISPLAY")) {
            if (qEnvironmentVariableIsSet("DISPLAY")) {
                qputenv("QT_QPA_PLATFORM", "xcb");
                fprintf(stderr, "pestudio: Wayland session -- using the xcb "
                                "backend so plugin editors can embed\n");
            } else {
                fprintf(stderr, "pestudio: Wayland session with no DISPLAY; "
                                "plugin editors need XWayland and will be "
                                "refused\n");
            }
        }
    }
    /* Isolate by default. This is a browser: it loads a hundred plugins in a
     * session, and one that faults would otherwise take the whole window down
     * and lose the user's place. TAL-U-No-62 does exactly that, in its own code,
     * every time. Behind the helper the crash costs a subprocess and the host
     * reports it. Editors still work -- they arrive as pixels through shared
     * memory -- except for native Linux plugins, which embed an X11 window and
     * are therefore kept in process by pehost regardless of this. */
    if (!qEnvironmentVariableIsSet("PEHOST_ISOLATE")) {
        pehost_set_isolation(1);
        fprintf(stderr, "pestudio: hosting plugins out-of-process "
                        "(PEHOST_ISOLATE=0 to disable)\n");
    }

    QApplication app(argc, argv);
    /* Must be installed before any plugin is opened: the Classic backend is handed
     * this when its shim is built, and without it a dial follows the click and
     * then nothing else. */
    pehost_set_input_pump(pump_input, nullptr);
    /* The positional argument is either a tree to browse or one plugin to open:
     *
     *   pestudio /path/to/linux/extracted            browse a tree
     *   pestudio fb799964.dll                        open one plugin
     *   pestudio fb799964.dll --patch glass-pad.json  ... with a patch applied
     *
     * Which it is comes from the filesystem rather than from a separate flag,
     * because a macOS plugin is a bundle -- a directory -- and so is a .vst3, so
     * "is it a directory" cannot decide it. pehost is asked instead: anything it
     * recognises as loadable is a plugin, and everything else is a root to scan.
     */
    QString dir, plugin, bankFile, pick;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cycle")) { i++; continue; }   /* takes a value */
        if (!strcmp(argv[i], "--patch") && i + 1 < argc) {
            bankFile = QString::fromLocal8Bit(argv[++i]);
            continue;
        }
        if (!strcmp(argv[i], "--pick") && i + 1 < argc) {
            pick = QString::fromLocal8Bit(argv[++i]);
            continue;
        }
        if (argv[i][0] != '-' && dir.isEmpty() && plugin.isEmpty() &&
            bankFile.isEmpty()) {
            const QString arg = QString::fromLocal8Bit(argv[i]);
            const QByteArray raw = QFileInfo(arg).absoluteFilePath().toLocal8Bit();
            char why[128] = "";
            if (arg.endsWith(".json", Qt::CaseInsensitive))
                bankFile = arg;
            else if (QFileInfo(arg).exists() &&
                     (pehost_can_load(raw.constData(), why, sizeof why) ||
                      pehost_is_classic_mac(raw.constData()) ||
                      pehost_is_native_vst2(raw.constData())))
                plugin = QFileInfo(arg).absoluteFilePath();
            else
                dir = arg;
        }
    }

    /* A bank names the plugin it was written for, so naming the bank is enough
     * to open both -- which is what makes a set of patches a thing you can hand
     * someone rather than a thing they have to be told how to load. */
    patch_bank *bank = nullptr;
    int startPatch = 0;
    if (!bankFile.isEmpty()) {
        char err[256];
        bank = patch_bank_read(bankFile.toLocal8Bit().constData(), err, sizeof err);
        if (!bank) {
            fprintf(stderr, "pestudio: %s\n", err);
            return 2;
        }
        if (patch_bank_is_multi_plugin(bank))
            fprintf(stderr, "pestudio: %s spans several plugins -- selecting a "
                            "patch loads the one it names\n",
                    qPrintable(QFileInfo(bankFile).fileName()));
        fprintf(stderr, "pestudio: %s -- %d patch(es) for %s\n",
                qPrintable(QFileInfo(bankFile).fileName()), patch_bank_count(bank),
                *patch_bank_plugin_name(bank) ? patch_bank_plugin_name(bank)
                                              : "an unnamed plugin");
        /* --pick takes a name or an index: a bank is read by people and driven
         * by scripts, and those want different handles on the same thing.
         *
         * Resolved before the plugin below, not after: in a cross-machine bank
         * each patch names its own plugin, so which patch was picked is what
         * decides which synth to open. */
        if (!pick.isEmpty()) {
            startPatch = -1;
            for (int i = 0; i < patch_bank_count(bank); i++)
                if (pick.compare(QString::fromLocal8Bit(patch_bank_patch_name(bank, i)),
                                 Qt::CaseInsensitive) == 0) { startPatch = i; break; }
            bool isNum = false;
            if (startPatch < 0) {
                int n = pick.toInt(&isNum);
                if (isNum && n >= 0 && n < patch_bank_count(bank)) startPatch = n;
            }
            if (startPatch < 0) {
                fprintf(stderr, "pestudio: no patch called \"%s\" in %s -- it has:\n",
                        qPrintable(pick), qPrintable(bankFile));
                for (int i = 0; i < patch_bank_count(bank); i++)
                    fprintf(stderr, "    %s\n", patch_bank_patch_name(bank, i));
                patch_bank_free(bank);
                return 2;
            }
        }
        if (plugin.isEmpty()) {
            /* The plugin the *starting* patch wants, which in a cross-machine
             * bank is not the bank default. */
            const char *pp = patch_bank_patch_plugin_path(bank, startPatch);
            if (*pp && QFileInfo(QString::fromLocal8Bit(pp)).exists())
                plugin = QString::fromLocal8Bit(pp);
            else if (*pp)
                fprintf(stderr, "pestudio: %s says its plugin is at %s, which is "
                                "not there -- name the plugin instead\n",
                        qPrintable(bankFile), pp);
            else
                fprintf(stderr, "pestudio: %s names no \"pluginPath\" -- name the "
                                "plugin as well\n", qPrintable(bankFile));
        }
    }
    fprintf(stderr, "pestudio: dir=\"%s\" plugin=\"%s\" bank=\"%s\"\n",
            qPrintable(dir), qPrintable(plugin), qPrintable(bankFile));
    fflush(stderr);
    Window w(dir, plugin, bankFile, bank, startPatch);
    w.show();
    /* --cycle <ms> walks the whole list unattended, opening each editor in
     * turn. Switching plugins with an editor attached is the failure-prone
     * path, and clicking through 90-odd of them by hand is not repeatable. */
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--cycle")) {
            int ms = (i + 1 < argc) ? atoi(argv[i + 1]) : 0;
            w.startCycle(ms > 0 ? ms : 1500);
            break;
        }
    return app.exec();
}
