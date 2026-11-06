#!/usr/bin/env python3
"""Compares C (-O0 and -O2), Python and adaC (.exe running under Wine). Best of N runs."""
import subprocess, time, os, sys
os.environ["WINEDEBUG"] = "-all"; os.environ["WINEPREFIX"] = "/tmp/wp"
HERE = os.path.dirname(os.path.abspath(__file__)); os.chdir(HERE)
ADAC = "../adac"; WINE = ["/usr/lib/wine/wine64"]
N = 3
def run(cmd):
    best, out = 1e9, ""
    for _ in range(N):
        t = time.perf_counter()
        r = subprocess.run(cmd, capture_output=True, text=True)
        best = min(best, time.perf_counter() - t); out = r.stdout.strip()
    return best, out
subprocess.run([ADAC, "../examples/hello.adac", "-o", "hello.exe"], capture_output=True)
base, _ = run(WINE + ["hello.exe"])
print(f"(Wine start-up discounted from adaC times: {base*1000:.0f} ms)\n")
print(f"{'test':<9}{'C -O2':>9}{'C -O0':>9}{'adaC':>9}{'Python':>9}   result (same?)")
rows = []
for name in ["fib", "loop", "pi", "sieve", "objects"]:
    subprocess.run(["gcc", "-O2", "-o", name + "_o2", name + ".c"], check=True)
    subprocess.run(["gcc", "-O0", "-o", name + "_o0", name + ".c"], check=True)
    c = subprocess.run([ADAC, name + ".adac", "-o", name + ".exe"], capture_output=True, text=True)
    if c.returncode: print(c.stderr); sys.exit(1)
    t2, o2 = run(["./" + name + "_o2"]); t0, o0 = run(["./" + name + "_o0"])
    tc, oc = run(WINE + [name + ".exe"]); tc = max(tc - base, 0.001)
    tp, op = run(["python3", name + ".py"])
    same = len({o2, o0, oc.replace("\r", ""), op}) == 1
    rows.append((name, t2, t0, tc, tp))
    print(f"{name:<9}{t2:>8.2f}s{t0:>8.2f}s{tc:>8.2f}s{tp:>8.2f}s   {o2} {'OK' if same else 'DIFFERENT: ' + str((o0, oc, op))}")
print("\nTimes slower than C -O2 (lower = better):")
print(f"{'test':<9}{'C -O0':>9}{'adaC':>9}{'Python':>9}")
for n, t2, t0, tc, tp in rows:
    print(f"{n:<9}{t0/t2:>8.1f}x{tc/t2:>8.1f}x{tp/t2:>8.1f}x")
