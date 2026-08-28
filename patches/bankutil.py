#!/usr/bin/env python3
"""Shared helper: the full parameter set of a plugin, as a base for patches.

A patch that lists only the parameters it cares about inherits the rest from
whatever program happens to be selected -- so the same patch is a different
sound depending on where you were standing when you picked it. Measured on
FB-7999: the "cursor" patch rendered peak 0.1137 from program 0 and 0.1294 from
program 40, different files entirely.

Pinning "program" fixes the inheritance but not the principle: the unlisted
parameters still come from that program rather than from the patch. So every
patch is instead filled out to the plugin's *complete* parameter set, and
selecting one overrides the program outright.

The base is read from the plugin itself at a known program, through the same
--save-patch that already round-trips byte-identically.
"""
import json, os, subprocess, tempfile


def full_param_base(peload, dll, program=0):
    """Every parameter of `dll` at `program`, as {name: value}.

    Keys are exactly what patch_load resolves: parameter names, or the index as
    a string where a plugin gives several parameters the same name (FB-3300 does
    it for 183 of 233). Returns {} if the plugin will not load.
    """
    fd, tmp = tempfile.mkstemp(suffix=".json", prefix="base-")
    os.close(fd)
    os.remove(tmp)
    try:
        subprocess.run([peload, dll, "--program", str(program),
                        "--save-patch", tmp],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=180)
    except subprocess.TimeoutExpired:
        return {}
    if not os.path.exists(tmp):
        return {}
    try:
        return json.load(open(tmp)).get("params", {})
    finally:
        os.remove(tmp)


def complete(patch_params, base, program=0):
    """A patch's parameters filled out to the plugin's whole set.

    The patch wins wherever it says anything; the base supplies the rest. Insert
    order is base-first so a generated file reads in the plugin's own parameter
    order rather than recipe order.
    """
    out = dict(base)
    out.update(patch_params)
    return out


def is_hand_tuned(bank):
    """True when a bank has been edited by hand and must not be regenerated.

    Everything here is generated, which means everything here can be
    regenerated -- and would silently destroy manual work. A bank carrying
    "handTuned": true is the author's, not the generator's: make_menu_banks
    skips it, and the repair, retune, fit and stop passes leave it alone.

    Set it by adding the key to the bank object:

        { "plugin": "...", "handTuned": true, "patches": [ ... ] }

    Remove it to hand the bank back to the generator.
    """
    return bool(bank.get("handTuned"))
