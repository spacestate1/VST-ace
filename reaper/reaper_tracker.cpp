// The tracker's song, imported into REAPER: one track per playing track, each
// with a MIDI item holding what the tracker would send, named as the song
// names it. Run from the Action list ("vst-ace: Import tracker song...").
//
// The notes come from trk_song_events -- the same walk the tracker's own MIDI
// export uses -- so this and File > Export MIDI agree, and what lands in
// REAPER is what the tracker plays. Muted tracks are left out, as there.

#define REAPERAPI_IMPLEMENT
#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_InsertTrackAtIndex
#define REAPERAPI_WANT_CountTracks
#define REAPERAPI_WANT_GetTrack
#define REAPERAPI_WANT_GetSetMediaTrackInfo_String
#define REAPERAPI_WANT_GetSetMediaItemTakeInfo_String
#define REAPERAPI_WANT_CreateNewMIDIItemInProj
#define REAPERAPI_WANT_GetActiveTake
#define REAPERAPI_WANT_MIDI_InsertNote
#define REAPERAPI_WANT_MIDI_InsertCC
#define REAPERAPI_WANT_MIDI_Sort
#define REAPERAPI_WANT_MIDI_GetPPQPosFromProjQN
#define REAPERAPI_WANT_SetCurrentBPM
#define REAPERAPI_WANT_GetUserFileNameForRead
#define REAPERAPI_WANT_Undo_BeginBlock2
#define REAPERAPI_WANT_Undo_EndBlock2
#define REAPERAPI_WANT_UpdateArrange
#define REAPERAPI_WANT_ShowMessageBox

#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "trk.h"
}

static int g_cmd;

static void say(const char *title, const char *fmt, const char *a = "")
{
    char m[4400];
    std::snprintf(m, sizeof m, fmt, a);
    ShowMessageBox(m, title, 0);
}

// What the song's tracks become. Returns the number of tracks made, -1 when
// the song could not be read.
static int import_song(const char *path)
{
    char err[256] = "";
    trk_song *s = (trk_song *)std::malloc(sizeof *s);
    if (!s) return -1;
    if (trk_song_load(s, path, err, sizeof err)) {
        say("Import tracker song", "Could not read the song: %s", err);
        std::free(s);
        return -1;
    }

    const bool empty = CountTracks(nullptr) == 0;
    const double beats = (double)trk_song_ticks(s) / TRK_MIDI_PPQ;     // quarter notes
    int made = 0;

    Undo_BeginBlock2(nullptr);
    for (int t = 0; t < TRK_TRACKS; t++) {
        trk_mev *ev;
        int n;
        if (s->track[t].mute) continue;
        if (trk_song_events(s, t, &ev, &n) < 0 || !n) { std::free(ev); continue; }

        InsertTrackAtIndex(CountTracks(nullptr), true);
        MediaTrack *tr = GetTrack(nullptr, CountTracks(nullptr) - 1);
        char name[TRK_NAME_LEN + 8];
        if (s->track[t].name[0]) std::snprintf(name, sizeof name, "%s", s->track[t].name);
        else std::snprintf(name, sizeof name, "Track %d", t + 1);
        GetSetMediaTrackInfo_String(tr, "P_NAME", name, true);

        const bool qn = true;                                          // positions in quarter notes
        MediaItem *item = CreateNewMIDIItemInProj(tr, 0.0, beats, &qn);
        MediaItem_Take *take = item ? GetActiveTake(item) : nullptr;
        if (take) {
            GetSetMediaItemTakeInfo_String(take, "P_NAME", name, true);
            const bool nosort = true;
            for (int i = 0; i < n; i++) {
                const double a = MIDI_GetPPQPosFromProjQN(take, (double)ev[i].start / TRK_MIDI_PPQ);
                if (ev[i].is_cc) {
                    MIDI_InsertCC(take, false, false, a, 0xB0, ev[i].chan, ev[i].a, ev[i].b);
                } else {
                    const double b = MIDI_GetPPQPosFromProjQN(take, (double)ev[i].end / TRK_MIDI_PPQ);
                    MIDI_InsertNote(take, false, false, a, b, ev[i].chan, ev[i].a, ev[i].b, &nosort);
                }
            }
            MIDI_Sort(take);
        }
        std::free(ev);
        made++;
    }

    // The tempo goes with the song, but a project that already has tracks
    // has its own: ask before changing it.
    if (made && s->bpm > 0) {
        bool set = empty;
        if (!set) {
            char m[200];
            std::snprintf(m, sizeof m, "Set the project tempo to %.2f BPM, as in the song?", s->bpm);
            set = ShowMessageBox(m, "Import tracker song", 4) == 6;    // MB_YESNO, IDYES
        }
        if (set) SetCurrentBPM(nullptr, s->bpm, false);
    }
    Undo_EndBlock2(nullptr, "Import tracker song", -1);
    UpdateArrange();
    std::free(s);
    return made;
}

static void run()
{
    char path[4096] = "";
    if (!GetUserFileNameForRead(path, "Import tracker song", "trk")) return;
    const int n = import_song(path);
    if (n == 0) say("Import tracker song", "The song has no playing tracks (muted tracks are left out).");
}

static bool on_command(int cmd, int)
{
    if (cmd != g_cmd) return false;
    run();
    return true;
}

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE, reaper_plugin_info_t *rec)
{
    if (!rec) return 0;                                  // unloading
    if (rec->caller_version != REAPER_PLUGIN_VERSION || !rec->GetFunc) return 0;
    if (REAPERAPI_LoadAPI(rec->GetFunc) != 0) return 0;  // a function we need is missing: do not load
    g_cmd = rec->Register("command_id", (void *)"VSTACE_IMPORT_TRACKER_SONG");
    if (!g_cmd) return 0;
    static gaccel_register_t accel = { { 0, 0, 0 }, "vst-ace: Import tracker song (.trk)..." };
    accel.accel.cmd = (unsigned short)g_cmd;
    rec->Register("gaccel", &accel);
    rec->Register("hookcommand", (void *)on_command);
    return 1;
}
