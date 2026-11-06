#!/usr/bin/env python3
"""
run_tests.py - adaC compiler test suite.

 1. Compiles every program in RUN_CASES, runs the generated .exe in the emulator
    (emu.py) and compares stdout / exit code with the expected output (.out).
 2. Checks that invalid programs are rejected with the right diagnostic.
Usage: python3 tests/run_tests.py [filter]
"""
import os, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from emu import run_pe

ADAC = os.path.join(ROOT, "adac")
OUT = os.path.join(HERE, "out")
os.makedirs(OUT, exist_ok=True)

def compile_src(path, exe):
    return subprocess.run([ADAC, path, "-o", exe, "--no-color"], capture_output=True, text=True)

# (file, stdin, expected exit code)
RUN_CASES = [
    ("basic.adac", b"", 0),
    ("classes.adac", b"", 0),
    ("manual.adac", b"", 0),
    ("gc_stress.adac", b"", 0),
    ("input.adac", b"Maria\n20\n1.75\n", 0),
    ("../../examples/test_fixed.adac", b"7\n", 0),
    ("../../examples/hello.adac", b"", 0),
    ("../../examples/bank.adac", b"", 0),
    ("../../examples/primes.adac", b"", 0),
    ("../../examples/all_gc.adac", b"5\n2.5\nMaria\n", 0),
    ("../../examples/all_manual.adac", b"", 0),
    ("../../examples/features.adac", b"", 0),
    ("../../examples/collections.adac", b"", 0),
    ("../../examples/advanced_oop.adac", b"", 0),
    ("../../examples/strings_enum_matrix.adac", b"", 0),
    ("../../examples/print_order_printf.adac", b"", 0),
    ("../../examples/modules/main.adac", b"", 0),
]

# (description, source code, expected text in the error output; None = must compile)
ERROR_CASES = [
    ("undeclared variable", "void main() { println(x); }", "variable 'x' is not declared"),
    ("type mismatch", 'void main() { int a = "hi"; }', "cannot convert 'String' to 'int'"),
    ("implicit float to int", "void main() { int a = 3.5; }", "use an explicit cast"),
    ("assign to const", "const A = 1;\nvoid main() { A = 2; }", "cannot modify constant 'A'"),
    ("missing return", "int f(int a) { if (a > 0) return 1; }\nvoid main() { }", "must return a value of type 'int'"),
    ("private field", "class A { private int x; }\nvoid main() { A a = new A(); a.x = 1; }", "is private in 'A'"),
    ("class inside function", "void main() { class C { } }", "classes cannot be declared inside functions"),
    ("setMen without #NOT_GARBAGE", "void main() { setMen(8); }", "#NOT_GARBAGE"),
    ("duplicate method", "class A { void f() {} void f() {} }\nvoid main() { }", "is already declared in this class"),
    ("unknown class", "void main() { Foo f = new Foo(); }", "unknown type 'Foo'"),
    ("wrong arity", "int f(int a) { return a; }\nvoid main() { f(1, 2); }", "expects 1 argument(s) but got 2"),
    ("break outside loop", "void main() { break; }", "'break' outside a loop"),
    ("method named like a global variable", "area = 1.5;\nclass A { float area() { return 2.0; } float f() { return area(); } }\nvoid main() { }", None),
    ("duplicate case", "void main() { int x = 1; switch (x) { case 1: break; case 1: break; } }", "duplicate 'case' label"),
    ("switch on float", "void main() { float f = 1.0; switch (f) { default: break; } }", "'switch' accepts int, char, bool or String"),
    ("non-constant case", "void main() { int x = 1; int y = 2; switch (x) { case y: break; } }", "must be a constant"),
    ("instanceof on int", "void main() { int x = 1; bool b = x instanceof String; }", "instanceof only applies"),
    ("empty literal without type", "void main() { a = {}; }", "an empty array literal needs a type"),
    ("pow with float exponent", "void main() { println(pow(2, 1.5)); }", "exponent must be an integer"),
    ("instantiate abstract class", "abstract class A { }\nvoid main() { A a = new A(); }", "abstract class"),
    ("interface not implemented", "interface I { void f(); }\nclass C implements I { }\nvoid main() { }", "does not implement method 'f' of interface 'I'"),
    ("abstract method not implemented", "abstract class A { abstract void f(); }\nclass C extends A { }\nvoid main() { }", "does not implement abstract method"),
    ("instance field in static method", "class A { int x; static int f() { return x; } }\nvoid main() { }", "static method"),
    ("strict mode", "#STRICT\nvoid main() { x = 1; }", "strict mode"),
    ("List without type", "void main() { List l = new List(); }", "needs an element type"),
    ("List wrong element type", 'void main() { List<int> l = new List<int>(); l.add("a"); }', "cannot convert 'String' to 'int'"),
    ("List<int> = List<String>", "void main() { List<int> a = new List<int>(); List<String> b = a; }", "different element types"),
    ("Map with int key", "void main() { Map<int, int> m; }", "Map requires a String key"),
    ("unknown enum constant", "enum E { A }\nvoid main() { println(E.B); }", "has no constant 'B'"),
    ("static via object", "class A { static void f() { } }\nvoid main() { A a = new A(); a.f(); }", "is static"),
    ("no main", "void f() { }", "function 'main' not found"),
    ("missing semicolon", "void main() { int a = 1 }", "expected ';'"),
    ("for(_) without declaration", "void main() { for (_; i <= 3; i++) { } }", "with 'for(_; ...)' the variable must exist"),
    ("catch of a non-Exception", "class A { }\nvoid main() { try { } catch (A a) { } }", "derived from Exception"),
    ("string times int", 'void main() { String s = "a"; int x = s * 2; }', "does not apply to types 'String' and 'int'"),
    ("printf arguments", 'void main() { printf("%d %d", 1); }', "expects more arguments"),
    ("circular inheritance", "class A extends B { } class B extends A { }\nvoid main() { }", "circular inheritance"),
    ("super not first", "class A { A(int x) {} } class B extends A { B() { int y = 1; super(2); } }\nvoid main() { }", "super(...) must be the first"),
    ("typo warning", "void main() { total = 1; println(total); totl = 2; println(totl); }", None),
]

def main():
    flt = sys.argv[1] if len(sys.argv) > 1 else ""
    ok = fail = 0
    for name, stdin, code in RUN_CASES:
        if flt not in name: continue
        src = os.path.join(HERE, "cases", name)
        exe = os.path.join(OUT, os.path.basename(name).replace(".adac", ".exe"))
        exp_path = os.path.join(HERE, "cases", os.path.basename(name).replace(".adac", ".out"))
        c = compile_src(src, exe)
        if c.returncode != 0:
            print(f"FAIL {name}: did not compile\n{c.stderr}"); fail += 1; continue
        r = run_pe(exe, stdin)
        got = r.stdout.decode("utf-8", "replace")
        want = open(exp_path, encoding="utf-8").read() if os.path.exists(exp_path) else None
        if want is None:
            open(exp_path, "w", encoding="utf-8").write(got)
            print(f"NEW  {name}: expectation recorded ({len(got)} bytes) - review it!"); continue
        if r.error or r.exit_code != code or got != want:
            print(f"FAIL {name}: exit={r.exit_code} error={r.error}\n--- got\n{got}\n--- expected\n{want}")
            fail += 1
        else:
            print(f"ok   {name}  (live heap at exit: {r.heap_live} B, {r.heap_allocs} allocations, {r.heap_frees} frees)")
            ok += 1
    for desc, src, needle in ERROR_CASES:
        if flt not in desc: continue
        with tempfile.NamedTemporaryFile("w", suffix=".adac", delete=False, dir=OUT) as f:
            f.write(src)
        c = compile_src(f.name, f.name + ".exe")
        os.unlink(f.name)
        if needle is None:
            if c.returncode != 0: print(f"FAIL should-compile[{desc}]\n{c.stderr}"); fail += 1
            else: print(f"ok   accepts[{desc}]"); ok += 1
            continue
        if c.returncode == 0 or needle not in c.stderr:
            print(f"FAIL error[{desc}]: expected '{needle}'\n{c.stderr}"); fail += 1
        else:
            print(f"ok   error[{desc}]"); ok += 1
    print(f"\n{ok} ok, {fail} failure(s)")
    sys.exit(1 if fail else 0)

main()
