"""Assertions for timing_check.ps1 (docs task 29 / section 9.2-Z).

Parses the "[GI 耗时] ... NAME=x.xxxms" lines that the sample logs when HE_GI_TIMING=1 and
checks that the readout is real: non-zero for a source that runs, exactly zero for a source
that has no pass, and scaling with the work (SSGI 16 vs 64 samples), plus repeatability.
"""
import re
import sys, os

LINE = re.compile(r"\[GI \u8017\u65f6\](.*)$")
ENTRY = re.compile(r"([A-Za-z_]+)=([0-9.]+)ms")


def read_timings(logpath):
    """Last '[GI 耗时]' line of a run -> {name: ms}"""
    last = None
    for line in open(logpath, encoding="utf-8", errors="replace").read().splitlines():
        m = LINE.search(line)
        if m:
            last = {k: float(v) for k, v in ENTRY.findall(m.group(1))}
    return last or {}


def main(outdir):
    fails = []
    t = {}
    for tag in ("ssgi16", "ssgi64", "ssgi16b", "nossgi"):
        t[tag] = read_timings(os.path.join(outdir, "timing_%s.log" % tag))
        print("%-8s %s" % (tag, {k: round(v, 4) for k, v in sorted(t[tag].items())}))

    # 1) a source that runs reports a real number; a source with no pass reports exactly 0
    if not (t["ssgi16"].get("SSGI", 0.0) > 0.05):
        fails.append("SSGI reports %.4f ms while running (expected > 0.05 ms): the readout is "
                     "still not real" % t["ssgi16"].get("SSGI", 0.0))
    if t["nossgi"].get("SSGI", 0.0) != 0.0:
        fails.append("SSGI reports %.4f ms although it has no pass in that config"
                     % t["nossgi"].get("SSGI", 0.0))
    if not (t["nossgi"].get("DDGI", 0.0) > 0.0):
        fails.append("DDGI reports 0 while it is the only diffuse source in that config")

    # 2) scaling the work scales the reading: 64 samples vs 16 samples
    a = t["ssgi16"].get("SSGI", 0.0)
    b = t["ssgi64"].get("SSGI", 0.0)
    ratio = b / a if a > 0 else float("nan")
    print("SSGI 16 -> 64 samples: %.4f -> %.4f ms (x%.2f)" % (a, b, ratio))
    if not (ratio >= 1.5):
        fails.append("quadrupling SSGI's sample count only changed the reading by x%.2f "
                     "(expected >= 1.5x): the readout does not respond to work" % ratio)

    # 3) repeatability of two identical runs
    a2 = t["ssgi16b"].get("SSGI", 0.0)
    spread = abs(a2 - a) / a if a > 0 else float("nan")
    print("repeat run: %.4f vs %.4f ms (spread %.1f%%)" % (a, a2, 100.0 * spread))
    if not (spread <= 0.30):
        fails.append("two identical runs differ by %.0f%% (recorded tolerance 30%%)"
                     % (100.0 * spread))

    for f in fails:
        print("FAIL: " + f)
    if fails:
        print("RESULT: FAIL")
        return 1
    print("RESULT: PASS (real readout: non-zero when running, zero when absent, scales with work)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
