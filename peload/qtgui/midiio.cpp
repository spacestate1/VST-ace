#include "midiio.h"

#include <QSocketNotifier>
#include <QString>
#include <alsa/asoundlib.h>
#include <poll.h>
#include <time.h>

MidiIo::MidiIo(QObject *parent) : QObject(parent) {}

void MidiIo::stopInput()
{
    stop_.store(true, std::memory_order_relaxed);
    if (reader_.joinable()) reader_.join();
    rtSink_ = nullptr;          /* nothing may be called after this returns */
}

MidiIo::~MidiIo()
{
    stopInput();

    detachNotifiers();
    if (seq_) snd_seq_close(seq_);
}

void MidiIo::detachNotifiers()
{
    for (QSocketNotifier *n : notifiers_) { n->setEnabled(false); delete n; }
    notifiers_.clear();
}

bool MidiIo::open(QString *err)
{
    /* DUPLEX so one client owns both ports and shows up as a single device to
     * everything else, which is what makes it behave like a normal synth. */
    if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
        seq_ = nullptr;
        if (err) *err = "ALSA sequencer unavailable";
        return false;
    }
    /* A second instance must not be called "pestudio" as well. ALSA allows
     * duplicate client names, so two of them give a tracker two entries called
     * "pestudio:pestudio in" with nothing to tell them apart, and picking the
     * wrong one looks exactly like MIDI not working. Number the later ones. */
    {
        QByteArray name = "pestudio";
        snd_seq_client_info_t *ci = nullptr;
        if (snd_seq_client_info_malloc(&ci) >= 0) {
            for (int n = 2; n < 32; n++) {
                bool taken = false;
                snd_seq_client_info_set_client(ci, -1);
                while (snd_seq_query_next_client(seq_, ci) >= 0)
                    if (snd_seq_client_info_get_client(ci) != snd_seq_client_id(seq_)
                        && name == snd_seq_client_info_get_name(ci)) { taken = true; break; }
                if (!taken) break;
                name = "pestudio " + QByteArray::number(n);
            }
            snd_seq_client_info_free(ci);
        }
        snd_seq_set_client_name(seq_, name.constData());
        clientName_ = QString::fromUtf8(name);
    }

    inPort_ = snd_seq_create_simple_port(seq_, "pestudio in",
                  SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                  SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_SYNTHESIZER);
    outPort_ = snd_seq_create_simple_port(seq_, "pestudio out",
                  SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                  SND_SEQ_PORT_TYPE_MIDI_GENERIC);
    if (inPort_ < 0 || outPort_ < 0) {
        if (err) *err = "could not create MIDI ports";
        snd_seq_close(seq_); seq_ = nullptr;
        return false;
    }

    attachNotifiers();
    stop_.store(false, std::memory_order_relaxed);
    reader_ = std::thread([this] { readerLoop(); });
    rescan();
    return true;
}

QString MidiIo::portName() const
{
    if (!seq_) return QString();
    return QString("%1:%2").arg(snd_seq_client_id(seq_)).arg(inPort_);
}

/* Input runs on its own thread rather than the Qt event loop.
 *
 * A socket notifier delivers through the GUI thread, so a note waits behind
 * whatever the interface is doing -- and the interface is repainting a plugin
 * editor sixty times a second. That is milliseconds of jitter added before the
 * event is even timestamped, on top of the block quantisation, and it is the
 * larger of the two. Reading here means the timestamp is taken when the event
 * arrives rather than when the GUI gets round to it.
 *
 * The Qt signals are still emitted for the widgets; being emitted from this
 * thread they are queued to the GUI thread automatically, which is exactly right
 * for a piano that can lag a frame. Anything that must not lag goes through
 * rtSink_, called directly below. */
void MidiIo::readerLoop()
{
    while (!stop_.load(std::memory_order_relaxed)) {
        int n = snd_seq_poll_descriptors_count(seq_, POLLIN);
        if (n <= 0) { struct timespec t = {0, 5000000}; nanosleep(&t, nullptr); continue; }
        QVector<pollfd> pfds(n);
        snd_seq_poll_descriptors(seq_, pfds.data(), unsigned(n), POLLIN);
        /* A timeout rather than an indefinite wait, so a rescan that changes the
         * descriptor set is picked up and shutdown does not hang. */
        int r = poll(pfds.data(), (nfds_t)n, 50);
        if (r > 0) drain();
    }
}

void MidiIo::attachNotifiers()
{
    /* Deliberately none: the reader thread is the only consumer. Two readers
     * calling snd_seq_event_input would race over the same queue. */
    detachNotifiers();
}

int MidiIo::rescan()
{
    if (!seq_) return 0;

    const unsigned needRead  = SND_SEQ_PORT_CAP_READ  | SND_SEQ_PORT_CAP_SUBS_READ;
    const unsigned needWrite = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
    int added = 0;

    snd_seq_client_info_t *ci = nullptr;
    snd_seq_port_info_t   *pi = nullptr;
    if (snd_seq_client_info_malloc(&ci) < 0) return 0;
    if (snd_seq_port_info_malloc(&pi) < 0) { snd_seq_client_info_free(ci); return 0; }

    sources_.clear();
    sinks_.clear();
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(seq_, ci) >= 0) {
        int cl = snd_seq_client_info_get_client(ci);
        if (cl == SND_SEQ_CLIENT_SYSTEM || cl == snd_seq_client_id(seq_)) continue;
        const char *cname = snd_seq_client_info_get_name(ci);

        /* Never auto-connect to another pestudio. Subscribing to everything
         * that can send is right for keyboards and trackers, but two instances
         * are both senders and both receivers: the second one to rescan grabs
         * the first one's output, and once the first rescans in turn the pair
         * is a closed loop with thru on. Chaining two of these is a reasonable
         * thing to want and a terrible thing to do by accident -- `aconnect`
         * still does it on purpose. */
        if (cname && !qstrncmp(cname, "pestudio", 8)) continue;

        snd_seq_port_info_set_client(pi, cl);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(seq_, pi) >= 0) {
            unsigned cap  = snd_seq_port_info_get_capability(pi);
            unsigned type = snd_seq_port_info_get_type(pi);
            int      pnum = snd_seq_port_info_get_port(pi);
            if (!(type & SND_SEQ_PORT_TYPE_MIDI_GENERIC)) continue;

            /* anything that can send becomes a source for us */
            if ((cap & needRead) == needRead) {
                bool fresh = snd_seq_connect_from(seq_, inPort_, cl, pnum) == 0;
                if (fresh) added++;
                /* EBUSY means already subscribed, which still counts as connected */
                sources_ << QString("%1:%2").arg(cname).arg(snd_seq_port_info_get_name(pi));
            }
            /* and anything that can receive is a possible sink */
            if ((cap & needWrite) == needWrite) {
                if (autoOut_) {
                    if (snd_seq_connect_to(seq_, outPort_, cl, pnum) == 0) added++;
                    sinks_ << QString("%1:%2").arg(cname).arg(snd_seq_port_info_get_name(pi));
                }
            }
        }
    }
    snd_seq_port_info_free(pi);
    snd_seq_client_info_free(ci);

    emit connectionsChanged();
    return added;
}

void MidiIo::send(int status, int d1, int d2)
{
    if (!seq_ || outPort_ < 0) return;

    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, uint8_t(outPort_));
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);

    const int ch = status & 0x0f;
    switch (status & 0xf0) {
    case 0x80: snd_seq_ev_set_noteoff(&ev, ch, d1, d2); break;
    case 0x90:
        if (d2 > 0) snd_seq_ev_set_noteon(&ev, ch, d1, d2);
        else        snd_seq_ev_set_noteoff(&ev, ch, d1, 0);
        break;
    case 0xB0: snd_seq_ev_set_controller(&ev, ch, d1, d2); break;
    case 0xC0: snd_seq_ev_set_pgmchange(&ev, ch, d1); break;
    /* Pitch bend is signed and centred on zero in ALSA, but arrives from MIDI
     * as two 7-bit halves biased at 8192. */
    case 0xE0: snd_seq_ev_set_pitchbend(&ev, ch, ((d2 << 7) | d1) - 8192); break;
    case 0xD0: snd_seq_ev_set_chanpress(&ev, ch, d1); break;
    case 0xA0: snd_seq_ev_set_keypress(&ev, ch, d1, d2); break;
    default: return;
    }
    /* Only the handoff to ALSA is serialised -- see outMx_. The event above is a
     * local, and the reader thread must not be made to wait on the GUI for
     * longer than one write() takes. */
    {
        std::lock_guard<std::mutex> g(outMx_);
        snd_seq_event_output_direct(seq_, &ev);
    }
}

void MidiIo::drain()
{
    if (!seq_) return;

    snd_seq_event_t *ev = nullptr;

    /* Drain on event_input_pending, not just on the file descriptor. ALSA
     * decodes a whole read into a userspace buffer, so events can be waiting
     * with the fd already quiet -- and QSocketNotifier is level-triggered on
     * the fd, so it would not fire again and those events would be stranded.
     * Passing 1 lets ALSA also pull anything new from the kernel. */
    while (snd_seq_event_input_pending(seq_, 1) > 0) {
        if (snd_seq_event_input(seq_, &ev) < 0 || !ev) break;

        /* Channel filter applies to voice messages only, so clock and sysex
         * still get through when a single channel is selected. */
        bool isVoice = ev->type == SND_SEQ_EVENT_NOTEON ||
                       ev->type == SND_SEQ_EVENT_NOTEOFF ||
                       ev->type == SND_SEQ_EVENT_CONTROLLER ||
                       ev->type == SND_SEQ_EVENT_PGMCHANGE ||
                       ev->type == SND_SEQ_EVENT_PITCHBEND ||
                       ev->type == SND_SEQ_EVENT_CHANPRESS ||
                       ev->type == SND_SEQ_EVENT_KEYPRESS;
        if (channel_ >= 0 && isVoice && ev->data.note.channel != channel_) continue;

        int ch = isVoice ? ev->data.note.channel : 0;
        int status = -1, d1 = 0, d2 = 0;

        switch (ev->type) {
        case SND_SEQ_EVENT_NOTEON:
            status = 0x90 | ch; d1 = ev->data.note.note; d2 = ev->data.note.velocity;
            if (d2 > 0) emit noteOn(d1, d2); else emit noteOff(d1);
            break;
        case SND_SEQ_EVENT_NOTEOFF:
            status = 0x80 | ch; d1 = ev->data.note.note; d2 = 0;
            emit noteOff(d1);
            break;
        case SND_SEQ_EVENT_CONTROLLER:
            status = 0xB0 | ch;
            d1 = ev->data.control.param & 0x7f;
            d2 = ev->data.control.value & 0x7f;
            break;
        case SND_SEQ_EVENT_PGMCHANGE:
            status = 0xC0 | ch;
            d1 = ev->data.control.value & 0x7f;
            emit programChange(d1);
            break;
        case SND_SEQ_EVENT_PITCHBEND: {
            int v = ev->data.control.value + 8192;      /* back to 0..16383 */
            if (v < 0) v = 0;
            if (v > 16383) v = 16383;
            status = 0xE0 | ch; d1 = v & 0x7f; d2 = (v >> 7) & 0x7f;
            break;
        }
        case SND_SEQ_EVENT_CHANPRESS:
            status = 0xD0 | ch; d1 = ev->data.control.value & 0x7f;
            break;
        case SND_SEQ_EVENT_KEYPRESS:
            status = 0xA0 | ch; d1 = ev->data.note.note; d2 = ev->data.note.velocity;
            break;
        /* System realtime. These carry no channel, which is why the filter above
         * only applies to voice messages, and they are what a sequencer uses to
         * tell the host its tempo and whether the song is rolling. Without them
         * every tempo-synced arpeggiator and delay runs at the host's default
         * rather than the sequencer's. */
        case SND_SEQ_EVENT_SONGPOS: {
            /* Sixteenths since the start, split across two 7-bit bytes. */
            int v = ev->data.control.value & 0x3FFF;
            status = 0xF2; d1 = v & 0x7f; d2 = (v >> 7) & 0x7f;
            break;
        }
        case SND_SEQ_EVENT_CLOCK:    status = 0xF8; break;
        case SND_SEQ_EVENT_START:    status = 0xFA; break;
        case SND_SEQ_EVENT_CONTINUE: status = 0xFB; break;
        case SND_SEQ_EVENT_STOP:     status = 0xFC; break;
        default:
            break;
        }

        if (status >= 0) {
            /* Straight to the host first, before any Qt machinery. */
            if (rtSink_) rtSink_(status, d1, d2);
            emit midi(status, d1, d2);
            /* Realtime is deliberately not echoed: thru exists to pass playing
             * through to other gear, and re-emitting a clock we were given is how
             * two devices end up driving each other. */
            if (thru_ && status < 0xF0) send(status, d1, d2);
        }
    }
}
