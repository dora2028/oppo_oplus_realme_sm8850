#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Wire (or unwire) the self-contained LZ4KDS feature into a kernel source tree.

The repository's own upstream files are never modified.  This script only
edits the *checked-out kernel tree* it is pointed at.

Revert is exact: before touching a file the script stores the original under
<kernel>/.lz4kds_backup/, and revert restores from that copy.  Every injected
block is additionally wrapped in LZ4KDS-BEGIN/END markers so a human (or the
`check` mode) can see exactly what was added.

Modes
-----
  inject   copy sources and wire them into the zram build (default)
  revert   restore originals and delete the copied files
  check    report what is currently applied; changes nothing

Usage
-----
  lz4kds_inject.py <kernel-dir> <lz4kds-dir> [--mode inject|revert|check]
"""
import argparse
import os
import shutil
import sys

B = "LZ4KDS-BEGIN"
E = "LZ4KDS-END"
BACKUP_DIR = ".lz4kds_backup"


def m_begin(lang="c"):
    if lang == "c":
        return "/* %s: added by lz4kds_inject.py */" % B
    return "# %s: added by lz4kds_inject.py" % B


def m_end(lang="c"):
    return "/* %s */" % E if lang == "c" else "# %s" % E


# Files copied into the kernel tree: (repo-relative, kernel-relative)
COPY_MAP = [
    ("src/lz4kds_sddc.c", "drivers/block/zram/lz4kds_sddc.c"),
    ("src/lz4kds_backend.c", "drivers/block/zram/lz4kds_backend.c"),
    ("src/lz4kd_encode.c", "drivers/block/zram/lz4kd_encode.c"),
    ("src/lz4kd_encode_delta.c", "drivers/block/zram/lz4kd_encode_delta.c"),
    ("src/lz4kd_decode.c", "drivers/block/zram/lz4kd_decode.c"),
    ("src/lz4kd_decode_delta.c", "drivers/block/zram/lz4kd_decode_delta.c"),
    ("src/lz4kd_private.h", "drivers/block/zram/lz4kd_private.h"),
    ("src/lz4kd_encode_private.h", "drivers/block/zram/lz4kd_encode_private.h"),
    ("include/lz4kds_sddc.h", "drivers/block/zram/lz4kds_sddc.h"),
    ("include/lz4kds_zcomp.h", "drivers/block/zram/lz4kds_zcomp.h"),
    ("include/lz4kd.h", "include/linux/lz4kd.h"),
]

# Codec objects shipped by the repo.  When the tree already provides them via
# the upstream lz4kd.patch (lib/lz4kd/), compiling these again would define
# lz4kd_encode()/lz4kd_decode()/... twice and fail at link time, so the copies
# are skipped and the upstream objects are used instead.
CODEC_OBJECTS = [
    "lz4kd_encode.o",
    "lz4kd_encode_delta.o",
    "lz4kd_decode.o",
    "lz4kd_decode_delta.o",
]

# Relative to the kernel tree root.
UPSTREAM_CODEC_DIR = "lib/lz4kd"

# Source basenames corresponding to CODEC_OBJECTS.
CODEC_OBJECTS_SRC = set(o[:-2] + ".c" for o in CODEC_OBJECTS)


def upstream_codec_present(root):
    """True when the tree already builds the LZ4KD codec itself."""
    probe = os.path.join(root, UPSTREAM_CODEC_DIR, "lz4kd_encode.c")
    return os.path.isfile(probe)

# Generated file, not copied from the repo.
DECL_HEADER = "drivers/block/zram/lz4kds_backend_decl.h"
DECL_BODY = """/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _LZ4KDS_BACKEND_DECL_H
#define _LZ4KDS_BACKEND_DECL_H
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_LZ4KDS)
extern const struct zcomp_ops backend_lz4kds;
#endif
#endif
"""

# Files this script edits, with the marker comment style used in each.
EDITED = [
    ("drivers/block/zram/Makefile", "make"),
    ("drivers/block/zram/zcomp.c", "c"),
    ("drivers/block/zram/zcomp.h", "c"),
    ("drivers/block/zram/zram_drv.c", "c"),
    ("drivers/block/zram/Kconfig", "kconfig"),
]


def die(msg):
    print("lz4kds: ERROR: " + msg, file=sys.stderr)
    sys.exit(1)


def read(path):
    with open(path, "r", encoding="utf-8") as fh:
        return fh.read()


def write(path, text):
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def backup_file(root, rel):
    """Keep the pristine copy once; never overwrite an existing backup."""
    src = os.path.join(root, rel)
    dst = os.path.join(root, BACKUP_DIR, rel)
    if os.path.exists(dst):
        return
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)


def restore_file(root, rel):
    src = os.path.join(root, BACKUP_DIR, rel)
    dst = os.path.join(root, rel)
    if not os.path.isfile(src):
        return False
    shutil.copyfile(src, dst)
    return True


# ---------------------------------------------------------------- inject ----

def copy_sources(root, repo):
    upstream = upstream_codec_present(root)
    n = 0
    for src_rel, dst_rel in COPY_MAP:
        # Skip the bundled codec sources when the tree already has them; the
        # header include/linux/lz4kd.h is still installed because the SDDC
        # layer needs the codec API declarations.
        base = os.path.basename(dst_rel)
        if upstream and base in CODEC_OBJECTS_SRC and \
                "/lib/" not in dst_rel and dst_rel.startswith("drivers/"):
            continue
        src = os.path.join(repo, src_rel)
        if not os.path.isfile(src):
            die("missing in repo: " + src_rel)
        dst = os.path.join(root, dst_rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        n += 1
    write(os.path.join(root, DECL_HEADER), DECL_BODY)
    print("lz4kds: copied %d sources (+1 generated header)" % n)


def wire_makefile(root):
    path = os.path.join(root, "drivers/block/zram/Makefile")
    text = read(path)
    if B in text:
        return False
    upstream = upstream_codec_present(root)
    lines = [
        "",
        m_begin("make"),
        "zram-$(CONFIG_ZRAM_BACKEND_LZ4KDS)\t+= lz4kds_backend.o lz4kds_sddc.o",
    ]
    if not upstream:
        lines += [
            "zram-$(CONFIG_ZRAM_BACKEND_LZ4KDS)\t+= lz4kd_encode.o lz4kd_encode_delta.o",
            "zram-$(CONFIG_ZRAM_BACKEND_LZ4KDS)\t+= lz4kd_decode.o lz4kd_decode_delta.o",
        ]
    lines += [
        "ccflags-$(CONFIG_ZRAM_BACKEND_LZ4KDS)\t+= -DLZ4K_DELTA",
        m_end("make"),
        "",
    ]
    text += "\n".join(lines)
    write(path, text)
    if upstream:
        print("lz4kds: %s provides the codec, not compiling the bundled copy"
              % UPSTREAM_CODEC_DIR)
    return True


def wire_zcomp_c(root):
    path = os.path.join(root, "drivers/block/zram/zcomp.c")
    text = read(path)
    if B in text:
        return False
    anchor = '#include "backend_842.h"\n'
    if anchor not in text:
        die("anchor not found in zcomp.c: " + anchor.strip())
    text = text.replace(anchor,
                        anchor + m_begin() + "\n"
                        + '#include "lz4kds_backend_decl.h"\n'
                        + m_end() + "\n", 1)

    sentinel = "\n\tNULL\n};"
    idx = text.find(sentinel)
    if idx < 0:
        die("backend table sentinel not found in zcomp.c")
    entry = ("\n" + m_begin() + "\n"
             "#if IS_ENABLED(CONFIG_ZRAM_BACKEND_LZ4KDS)\n"
             "\t&backend_lz4kds,\n"
             "#endif\n" + m_end())
    write(path, text[:idx] + entry + text[idx:])
    return True


def wire_zcomp_h(root):
    """Deliberately a no-op.

    The previous revision extended struct zcomp_ops with an lz4kds_release
    callback so zram_free_page() could notify the backend.  Since references
    are immortal there is nothing to notify, so the extension is dropped and
    struct zcomp_ops stays exactly as upstream defines it.
    """
    return False



def wire_zram_drv_c(root):
    """Deliberately a no-op.

    References are immortal: an alias/delta payload may already sit on the
    backing device and only carries a numeric reference id, so freeing a zram
    slot must never drop a reference.  There is therefore nothing for
    zram_free_page() to tell the backend.

    An earlier revision did inject a hook there that mapped every object only
    to decrement an accounting counter nothing ever read.  That ran on the
    free hot path, under the slot lock, in atomic context (zram_slot_free_
    notify is called from bio completion), costing a zcomp_stream_get() plus
    zs_map_object() round trip per freed page for no benefit.  Removing it is
    part of the latency fix.
    """
    return False



def wire_kconfig(root):
    path = os.path.join(root, "drivers/block/zram/Kconfig")
    text = read(path)
    if B in text:
        return False
    anchor = "config ZRAM_BACKEND_ZSTD"
    if anchor not in text:
        die("anchor not found in Kconfig: config ZRAM_BACKEND_ZSTD")
    upstream = upstream_codec_present(root)
    block = "\n".join([
        m_begin("kconfig"),
        "config ZRAM_BACKEND_LZ4KDS",
        '\tbool "lz4kds (LZ4KD + SDDC) compression support"',
        "\tdepends on ZRAM",
    ] + ([
        "\tselect LZ4KD_COMPRESS",
        "\tselect LZ4KD_DECOMPRESS",
    ] if upstream else []) + [
        "\thelp",
        "\t  LZ4KD plus SDDC similarity/delta compression: identical pages are",
        "\t  stored once as an alias, and pages differing in only a few bytes are",
        "\t  stored as a delta against a resident reference.  Pages matching",
        "\t  nothing fall back to plain single-page LZ4KD.",
        m_end("kconfig"),
        "",
        "",
    ])
    write(path, text.replace(anchor, block + anchor, 1))
    return True


def do_inject(root, repo):
    for rel, _ in EDITED:
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            die("missing kernel file: " + rel)
        backup_file(root, rel)

    copy_sources(root, repo)
    changed = 0
    for fn in (wire_makefile, wire_zcomp_c, wire_zcomp_h,
               wire_zram_drv_c, wire_kconfig):
        if fn(root):
            changed += 1
            print("lz4kds: wired " + fn.__name__.replace("wire_", ""))
        else:
            print("lz4kds: already wired " + fn.__name__.replace("wire_", ""))
    print("lz4kds: inject complete (%d files newly edited)" % changed)


# ---------------------------------------------------------------- revert ----

def strip_markers(text, lang):
    """Fallback path: drop injected blocks when no backup exists."""
    b, e = m_begin(lang), m_end(lang)
    removed = False
    while True:
        i = text.find(b)
        if i < 0:
            break
        j = text.find(e, i)
        if j < 0:
            die("begin marker without end marker")
        j += len(e)
        if text[j:j + 1] == "\n":
            j += 1
        text = text[:i] + text[j:]
        removed = True
    return text, removed


def do_revert(root, repo):
    for rel, lang in EDITED:
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            continue
        if restore_file(root, rel):
            print("lz4kds: restored " + rel)
        else:
            text, removed = strip_markers(read(path), lang)
            if removed:
                write(path, text)
                print("lz4kds: stripped markers from " + rel)
            else:
                print("lz4kds: nothing to revert in " + rel)

    for _, dst_rel in COPY_MAP:
        path = os.path.join(root, dst_rel)
        if os.path.isfile(path):
            os.remove(path)
    decl = os.path.join(root, DECL_HEADER)
    if os.path.isfile(decl):
        os.remove(decl)

    bdir = os.path.join(root, BACKUP_DIR)
    if os.path.isdir(bdir):
        shutil.rmtree(bdir)
    print("lz4kds: revert complete")


# ----------------------------------------------------------------- check ----

def do_check(root, repo):
    wired = 0
    for rel, _ in EDITED:
        path = os.path.join(root, rel)
        if os.path.isfile(path) and B in read(path):
            print("wired   " + rel)
            wired += 1
        else:
            print("clean   " + rel)

    present = sum(1 for _, d in COPY_MAP
                  if os.path.isfile(os.path.join(root, d)))
    backup = os.path.isdir(os.path.join(root, BACKUP_DIR))
    print("sources present: %d/%d, backup: %s"
          % (present, len(COPY_MAP), "yes" if backup else "no"))
    print("lz4kds: %s" % ("APPLIED" if wired or present else "NOT APPLIED"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("kernel_dir")
    ap.add_argument("lz4kds_dir")
    ap.add_argument("--mode", choices=["inject", "revert", "check"],
                    default="inject")
    a = ap.parse_args()

    if not os.path.isdir(a.kernel_dir):
        die("kernel tree not found: " + a.kernel_dir)
    if a.mode != "revert" and not os.path.isdir(a.lz4kds_dir):
        die("lz4kds dir not found: " + a.lz4kds_dir)

    if a.mode == "inject":
        do_inject(a.kernel_dir, a.lz4kds_dir)
    elif a.mode == "revert":
        do_revert(a.kernel_dir, a.lz4kds_dir)
    else:
        do_check(a.kernel_dir, a.lz4kds_dir)


if __name__ == "__main__":
    main()
