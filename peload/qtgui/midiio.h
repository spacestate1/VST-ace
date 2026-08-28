/* ALSA sequencer MIDI in and out for pestudio.
 *
 * Two ports on one client, the same shape the DW-8000 GUI exposes:
 *
 *   "pestudio in"   writable  -- a USB keyboard or another app (Furnace,
 *                                Schism, a DAW) connects here and plays us
 *   "pestudio out"  readable  -- everything played locally (on-screen piano,
 *                                computer keyboard, incoming MIDI if thru is
 *                                on) is echoed here, so pestudio can drive
 *                                hardware or another synth
 *
 * Input is event-driven: ALSA hands us poll descriptors and a QSocketNotifier
 * sits on each, so events arrive through the Qt event loop as they happen
 * rather than waiting on a polling interval. */
#ifndef PESTUDIO_MIDIIO_H
#define PESTUDIO_MIDIIO_H

#include <QObject>
#include <QStringList>
#include <QList>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

class QSocketNotifier;
struct _snd_seq;

class MidiIo : public QObject {
    Q_OBJECT
public:
    explicit MidiIo(QObject *parent = nullptr);
    ~MidiIo() override;

    /* Open the sequencer, create both ports and subscribe to every hardware
     * input found. False with a reason if the sequencer is unavailable. */
    bool open(QString *err);

    /* Re-scan for sources and sinks, subscribing to anything new, so a keyboard
     * plugged in after startup works without a restart. Returns how many new
     * connections were made. */
    int  rescan();

    bool        isOpen() const { return seq_ != nullptr; }
    QStringList sources() const { return sources_; }   /* connected inputs  */
    QStringList sinks()   const { return sinks_; }     /* connected outputs */
    QString     portName() const;                      /* "128:0" style     */

    /* The ALSA client name we actually got. Normally "pestudio"; a second
     * instance is "pestudio 2", and a tracker's device list shows that name, so
     * the window has to be able to say which one it is. */
    QString     clientName() const { return clientName_; }

    /* -1 accepts every channel; 0-15 accepts only that one. Applies to voice
     * messages on input only. */
    void setChannelFilter(int ch) { channel_ = ch; }
    int  channelFilter() const { return channel_; }

    /* Echo incoming MIDI straight back out. Off by default: with a keyboard
     * whose output is also connected to our input this would loop. */
    void setThru(bool on) { thru_ = on; }
    bool thru() const { return thru_; }

    /* Auto-subscribe our output to hardware sinks during rescan(). Off by
     * default, since most users want to hear the plugin, not drive a synth. */
    void setAutoConnectOut(bool on) { autoOut_ = on; }

    /* Send one raw MIDI voice message out. No-op if the port is closed. */
    void send(int status, int d1, int d2);

    /* Stop the reader thread and drop the realtime sink, without closing the
     * ports. Must be called before whatever the sink calls into is destroyed:
     * this object is a QObject child, and children are deleted by ~QObject after
     * the owner's own members are already gone. Idempotent. */
    void stopInput();

    /* A sink called the instant an event is decoded, on the reader thread rather
     * than through the Qt event loop.
     *
     * The signals below are queued to the GUI thread, which is right for updating
     * a piano widget and wrong for playing a note: whatever the interface happens
     * to be doing -- repainting a plugin editor sixty times a second, for
     * instance -- lands on top of the timing. Anything that must be sample-accurate
     * goes here instead; it must be safe to call from another thread, which
     * pehost's event queue is.
     *
     * Set before open(). */
    void setRealtimeSink(std::function<void(int, int, int)> f) { rtSink_ = std::move(f); }

signals:
    /* Raw status byte including channel, then the data bytes. */
    void midi(int status, int d1, int d2);
    void noteOn(int note, int vel);
    void noteOff(int note);
    void programChange(int program);
    void connectionsChanged();

private slots:
    void drain();

private:
    /* Reads ALSA directly, so nothing the GUI does can delay a note. */
    void readerLoop();

private:
    void attachNotifiers();
    void detachNotifiers();

    std::function<void(int, int, int)> rtSink_;
    std::thread              reader_;
    std::atomic<bool>        stop_{false};

    /* send() runs on two threads -- the GUI's, for the on-screen piano and the
     * computer keyboard, and the reader's, for thru -- and ALSA's sequencer
     * output path does no locking of its own: snd_seq_event_output_direct()
     * memcpys into the handle's output buffer and bumps the used count, with
     * nothing serialising two writers. Play a note locally while a keyboard is
     * feeding thru and the two interleave inside that buffer. */
    std::mutex               outMx_;

    struct _snd_seq         *seq_  = nullptr;
    int                      inPort_  = -1;
    int                      outPort_ = -1;
    int                      channel_ = -1;
    bool                     thru_    = false;
    bool                     autoOut_ = false;
    QStringList              sources_, sinks_;
    QString                  clientName_;
    QList<QSocketNotifier *> notifiers_;
};

#endif /* PESTUDIO_MIDIIO_H */
