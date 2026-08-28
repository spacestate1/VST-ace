/* patch -- plugin patches as JSON, singly or as a bank.
 *
 * The format is the plugin's own vocabulary: parameter *names* exactly as the
 * plugin reports them, values in the 0..1 the VST APIs use.
 *
 *   {
 *     "plugin":     "FB-7999",
 *     "uniqueID":   "0x66623739",
 *     "pluginPath": "../windows/VST2-64/fb799964.dll",
 *     "patches": [
 *       { "name": "cursor",  "program": 0, "params": { "VCF Cutoff": 0.61 } },
 *       { "name": "confirm", "params": { "VCF Cutoff": 0.80 } }
 *     ]
 *   }
 *
 * A file holding one patch writes "program" and "params" at the top level with
 * no "patches" array; it reads back as a bank of one, so callers handle a single
 * patch and a bank of forty through the same path.
 *
 * Keying on names rather than indices is what makes a patch readable, diffable
 * and hand-editable, and it survives a plugin being rebuilt with a parameter
 * inserted in the middle -- which an index-keyed file would not.
 *
 * A key that is all digits is taken as a parameter index, which covers the two
 * cases a name cannot: a parameter the plugin leaves unnamed, and several
 * parameters sharing one name (common -- FB-3300 does it for 183 of its 233).
 * Names are resolved first, so a plugin with a parameter genuinely called "12"
 * still wins.
 *
 * "plugin" and "uniqueID" are advisory, recorded so that applying a patch to
 * the wrong plugin can say so rather than silently scattering values across
 * unrelated parameters. "pluginPath" is what lets a bank name its own synth, so
 * that opening the bank opens the plugin it was written for; it is resolved
 * relative to the bank file, which keeps a bank portable alongside its plugin.
 */
#ifndef PATCH_H
#define PATCH_H

#include "pehost.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct patch_bank patch_bank;

/* Read a patch file, in either shape. Returns NULL on failure with a one-line
 * reason in `err`. */
patch_bank *patch_bank_read(const char *path, char *err, int errn);
void        patch_bank_free(patch_bank *b);

int         patch_bank_count(const patch_bank *b);
/* Never NULL: a patch that named itself nothing is numbered instead. */
const char *patch_bank_patch_name(const patch_bank *b, int i);

/* What the bank says it is for. Each is "" when the file did not say.
 * patch_bank_plugin_path() is absolute, having been resolved against the
 * directory the bank was read from. */
const char *patch_bank_plugin_name(const patch_bank *b);
const char *patch_bank_plugin_id(const patch_bank *b);
const char *patch_bank_plugin_path(const patch_bank *b);

/* The plugin patch `i` wants, which is the bank's unless the patch names its
 * own "pluginPath". A patch carrying one is what lets a single file span
 * several machines: selecting it means "load that plugin, then apply this" --
 * so the same sound can be compared across synths without opening a file per
 * synth. Returns "" when neither says. Always absolute. */
const char *patch_bank_patch_plugin_path(const patch_bank *b, int i);

/* True when the patches do not all name the same plugin, so a caller can tell a
 * one-machine bank from a cross-machine one without walking it. */
int patch_bank_is_multi_plugin(const patch_bank *b);

/* Apply patch `i` to an open plugin. Returns 0 on success, -1 if `i` is out of
 * range. `err` receives a warning on success when the bank names a different
 * plugin than the one it was applied to, so a caller wanting to be strict can
 * check for a non-empty `err` even at 0.
 *
 * `applied` and `missed` (either may be NULL) report how many keys matched a
 * parameter and how many did not. A patch that misses everything is the usual
 * sign of its belonging to another plugin. */
int patch_bank_apply(const patch_bank *b, int i, pehost *h,
                     char *err, int errn, int *applied, int *missed);

/* The same, but leaving the plugin's program where it is.
 *
 * For re-asserting a patch after the *user* changed program. Selecting a
 * program makes the plugin overwrite every parameter, which would silently
 * discard the patch -- so the patch is applied again on top. Its own "program"
 * is skipped here, because forcing the selection back would undo the change the
 * user just made in front of them. Since a patch carries the plugin's whole
 * parameter set, the result is the patch's sound either way. */
int patch_bank_apply_params(const patch_bank *b, int i, pehost *h,
                            char *err, int errn, int *applied, int *missed);

/* Read `path` and apply its first patch: the whole of the common case. */
int patch_load(pehost *h, const char *path, char *err, int errn,
               int *applied, int *missed);

/* Write the plugin's current program and every parameter to `path` as a
 * single-patch file. `plugin_path`, when given, is recorded as "pluginPath" so
 * the patch can later be opened without naming the plugin again. */
int patch_save(pehost *h, const char *path, const char *plugin_path,
               char *err, int errn);

#ifdef __cplusplus
}
#endif

#endif /* PATCH_H */
