/* FB-02 (Yamaha FB-01) program decoding.
 *
 * The layout is derived in ../../FINDINGS-FB02.md: the FXB container is shared
 * with every other Full Bucket plugin, but the 158-byte program body mirrors
 * the FB-01's own packed voice -- one byte per parameter -- rather than the
 * run of doubles FB-7999 uses. */
#ifndef FB02_H
#define FB02_H

#define FB02_BODY   158
#define FB02_OPS      4
#define FB02_OPSTRIDE 17

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int enable, level, velocity, boost, frequency, inharmonic, detune;
    int ks_type, level_adjust, ks_depth, ks_rate;
    int attack, attack_vel, decay1, decay2, sustain, release;
    int wave;          /* the plugin's own extension; see below */
} fb02_op;

typedef struct {
    int algorithm, transpose, pb_range, portamento, feedback, mode, pmd_ctrl;
    int out_l, out_r;
    int lfo_enable, lfo_wave, lfo_speed, lfo_sync;
    int lfo_am_depth, lfo_am_sens, lfo_pm_depth, lfo_pm_sens;
    fb02_op op[FB02_OPS];
} fb02_program;

/* `body` is the 158-byte program body from a decoded bank record.
 * Returns 0 on success.
 *
 * Note on `wave`: the four "OP%i: Wave" parameters live at p02..p05 and are
 * NOT present in the packed body. The FB-01 is sine-only -- the eight TX81Z
 * waveforms are FB-02's own addition, listed in its 1.1.0 release notes -- so
 * every factory program is sine and this field decodes to 0. */
int fb02_decode(fb02_program *p, const unsigned char *body, int body_len);

const char *fb02_global_name(int i);   /* 0..16, or NULL */
const char *fb02_op_name(int i);       /* 0..16, or NULL */

#ifdef __cplusplus
}
#endif

#endif /* FB02_H */
