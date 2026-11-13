#!/usr/bin/env python3
"""Render vgmrips' Daytona USA pack (45 MultiPCM logs) to 22,050 Hz mono WAVs for the EspRacer port.

    python3 -I tools/esp32/build_music.py ARCHIVE.zip OUTDIR [--rate 22050] [--jobs N]

OUTDIR/music/tNN.wav and OUTDIR/music/tracks.tsv (number, title, samples, loop start in samples or -1,
gain used). Builds tools/esp32/vgm_render.cpp into OUTDIR/work. A track whose peak would clip is
rendered again with a lower gain (target peak 31000). Output stays under build/: it is derived from
the pack, which the repository does not carry.
"""
import concurrent.futures, os, subprocess, sys, zipfile

here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.dirname(os.path.dirname(here))


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    archive, out = os.path.abspath(args[0]), os.path.abspath(args[1])
    rate, jobs = "22050", os.cpu_count() or 4
    for flag, val in zip(args[2::2], args[3::2]):
        if flag == "--rate": rate = val
        elif flag == "--jobs": jobs = int(val)
    work, music = os.path.join(out, "work"), os.path.join(out, "music")
    os.makedirs(work, exist_ok=True); os.makedirs(music, exist_ok=True)
    tool = os.path.join(work, "vgm_render")
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-O2", "-I", os.path.join(repo, "src"),
                    os.path.join(here, "vgm_render.cpp"), os.path.join(repo, "src/runtime/multipcm.cpp"),
                    "-lz", "-o", tool], check=True)
    tracks = []
    with zipfile.ZipFile(archive) as z:
        for name in sorted(z.namelist()):
            if name.endswith(".vgz") and name[:2].isdigit():
                dst = os.path.join(work, name)
                with open(dst, "wb") as f: f.write(z.read(name))
                tracks.append((int(name[:2]), name[3:-4], dst))
    if len(tracks) != 45:
        sys.exit(f"expected 45 recordings, found {len(tracks)}")

    def render(t):
        num, title, src = t
        wav = os.path.join(music, f"t{num:02d}.wav")
        gain = 1.0
        for _ in range(2):
            r = subprocess.run([tool, src, wav, "--rate", rate, "--gain", f"{gain:g}"], capture_output=True, text=True, check=True)
            peak, samples, loop = (float(x) for x in r.stdout.split()[1::2])
            if peak <= 31500: break
            gain = round(gain * 31000 / peak, 3)
        return num, title, int(samples), int(loop), gain

    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        rows = sorted(pool.map(render, tracks))
    with open(os.path.join(music, "tracks.tsv"), "w") as f:
        for num, title, samples, loop, gain in rows:
            f.write(f"{num}\t{title}\t{samples}\t{loop}\t{gain:g}\n")
    total = sum(r[2] for r in rows)
    print(f"{len(rows)} tracks, {total / int(rate):.0f} s, {total * 2 / 1e6:.1f} MB in {music}")


main()
