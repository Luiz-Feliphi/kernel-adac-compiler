# adaC — the adaC language compiler (`adac`)

A compiler written in **C** that turns `.adac` programs directly into **64-bit Windows executables**
(`.exe`, PE32+ format). There is no intermediate C step and no external assembler or linker:
`adac` generates the x86-64 machine code and writes the `.exe` byte by byte.

```
.adac source → lexer → parser (AST) → semantic analysis → x86-64 code generator → PE writer → program.exe
```

## Building the compiler

The compiler is portable C (GNU C11): it builds on Linux/macOS/Windows and **always produces Windows `.exe` files**.

```bash
make              # Linux / macOS / MSYS2            → ./adac
build.bat         # Windows with MinGW-w64 (gcc)     → adac.exe
```

## Using it

```bash
./adac examples/hello.adac                 # produces hello.exe
./adac program.adac -o out.exe
./adac program.adac --check                # only check (no .exe)
./adac program.adac --strict               # variables must be declared (same as #STRICT)
./adac program.adac --tokens               # print the tokens
./adac --dump-prelude [--manual]            # print the embedded runtime
```

Diagnostics show file, line, column and the source line with a `^`; the compiler keeps going after the
first error (up to 50) so several problems are reported at once. Everything the compiler prints is in English.
On success it prints the output size and the total compile time, e.g. `compiled: hello.exe (26112 bytes, garbage-collected memory) in 2.98 ms`.

## The language

| Aspect | How it works |
|---|---|
| Types | `int`/`long` (64 bits), `float`/`double` (IEEE 754, 64 bits), `bool`, `char`, `String`, `void`, `ptr`, `T*`, `T[]`, classes |
| Variables | `int x = 1;`  `const PI = (float) 3.14;`  `var n = 5;`  or **inference by assignment**: `area = PI * 4;`. The compiler **warns** when a name looks like a typo of another variable (`areaa` vs `area`) or when an implicitly declared variable is never read; `#STRICT` (or `--strict`) turns assignment-declarations into errors |
| Scope | Top-level statements become *globals* and run before `main`; inside blocks the scope is the block |
| Operators | `+ - * / %`, `& \| ^ ~ << >>`, `&& \|\| !`, comparisons, `?:`, `+= -= …`, `++ --` |
| Strings | `+` concatenates any type; `==` compares contents; `length() charAt() substring() equals() indexOf() trim() toUpperCase() toLowerCase() isEmpty() contains() startsWith() endsWith() replace() split() repeat()`, `s[i]` |
| Output | `print(a, b, c)` (comma concatenates; **all arguments are evaluated before printing**), `println(...)`, `printf("%5d %-5d %05d %8.2f %c %s %x %p %b", ...)` with width, `-` and `0` flags |
| Input | `read()` — the type comes from the target (`int n = read();`); also `readInt()`, `readFloat()`, `readLine()` |
| Control flow | `if/else`, `while`, `do-while`, `for`, `for (T x : array_or_list)`, `switch/case/default` (int, char, bool and **String**, with fall-through), `break`, `continue`, `return` (braces optional) |
| Functions | `int sum(int a, int b) {…}`; overloading by **number** of parameters; `main` is `void` or `int` |
| OOP | `class`, fields, methods, constructors (a method with no type named like the class), `this`, `new`, `extends`, `super(...)`, `super.m()`, virtual dispatch, `x instanceof Class` |
| More OOP | `static` fields and methods (`Util.twice(2)`, `Base.created`), `abstract class` / abstract methods, `interface` (with multiple `extends`), `class A extends B implements I, J` |
| Access | `public`, `private` (class only), `protected` (class and subclasses), `package`/omitted |
| Exceptions | `throw`, `try / catch (T e) / finally` (`T` derives from `Exception`; `RuntimeException` already exists) |
| Arrays | `int[] v = new int[10];` `int[] w = {1,2,3};` `new String[]{"a","b"}`, matrices `new int[3][4]` / `{{1},{2,3}}`, `v[i]`, `v.length` — bounds-checked |
| Collections | `List<T>` (`add push_back get set insert removeAt pop size indexOf contains`, `l[i]`, `l[i] = x`, for-each) and `Map<String,T>` (`put get has remove size keys`, `m["k"]`, `m["k"] = v`) |
| Math | `abs`, `min`, `max`, `pow(base, intExponent)`, `sqrt` |
| Enum / modules | `enum Color { A, B = 5, C }` used as `Color.A` (an `int` with names); `import "other.adac";` |

### The three `for` forms

```c
for (int i = 0 to 10; i++) {...}   // i from 0 to 10 (inclusive) — same as  i <= 10
for (int i to 10; i++)   {...}     // starts at 0 by default
int k = 0;  for (_; k <= 10; k++)  // uses a variable that already exists
for (int _; j <= 10; j++)          // DECLARES 'j' (initial value 0), lives only inside the for
```
Forgetting to declare the variable in `for (_; ...)` produces an explanatory error.

### Memory: garbage collector **or** manual

* **Default:** garbage collector. A *conservative mark-and-sweep* collector written in adaC
  (`runtime/gc.adac`): roots are the globals and the stack; it runs when the heap grows past a limit that
  adapts to the amount of live data.
* **`#NOT_GARBAGE`** anywhere at the top level of the file: the collector disappears, the runtime uses direct
  allocation and you release everything with `freeMEM`. Only in this mode do these exist:

```c
#NOT_GARBAGE
int* v = (int*) setMen(5 * sizeof(int));   // malloc
v[2] = 7;  int* p = v;  p = p >> 2;         // >> and << move pointers (in elements)
*p = *p + 1;  int y = 3;  int* py = &y;     // * dereferences, & takes the address
freeMEM(v);                                 // free (works for objects and Strings too)
```
Using `setMen`/`freeMEM` without the directive is a compile error.
(The older spelling `#NOT_GARGBAGE` is still accepted as an alias.)

### Directives

* `#NOT_GARBAGE` — see above.
* `#STRICT` — assignment to an undeclared name is an error.
* `#definePackage: ClassA, ClassB;` (alias `#definePackege`) — marks exported classes; the compiler checks that they exist.

## How the `.exe` is produced

| Module | Role |
|---|---|
| `src/lexer.c` | tokens, `//` and `/* */` comments, escapes, hex/binary literals, directives |
| `src/parser.c` | recursive descent + precedence, error recovery (`setjmp`) |
| `src/sema.c` | scopes, types, classes/vtables/itables, access control, implicit conversions; rewrites `print`, `printf` and string `+` into runtime calls |
| `src/x64.c` | x86-64 instruction encoder (REX, ModRM, SIB, SSE2) with labels and fixups |
| `src/codegen.c` | AST → machine code (convention: result in `RAX`, `double` as bits in `RAX`) |
| `src/pe.c` | PE32+ headers, `.text/.data/.idata` sections, `KERNEL32.dll` imports, address resolution |
| `runtime/*.adac` | runtime **written in adaC** (printing, strings, conversions, input, exceptions, collections, GC/allocator) compiled together with every program |

The executable imports only 8 functions from `KERNEL32.dll` (`ExitProcess`, `GetStdHandle`, `WriteFile`,
`ReadFile`, `GetProcessHeap`, `HeapAlloc`, `HeapFree`, `SetConsoleOutputCP`). Typical size: 12–20 KB.
Exceptions use a handler chain stored in the frame (`rbp/rsp` saved); `finally` also runs on `return`/`break`/`continue`.

## Tests

```bash
pip install unicorn capstone pefile
make test        # 56 tests
```

Because development happened without Windows, `tests/emu.py` **loads the generated PE `.exe` into the Unicorn
CPU emulator**, simulates the 8 `KERNEL32` functions (including the Windows x64 stack-alignment rule) and compares
the output. The suite covers arithmetic, floats, strings, classes and inheritance, interfaces, exceptions with
`finally`, arrays, collections, input, GC under stress (~6 MB of garbage), manual mode with pointers, and 39
compiler diagnostics. The binaries were also run under Wine.

> **Note:** execution on real Windows has not been tested in the environment where this was developed.
> The PE structure was validated with `pefile`/`objdump` (no warnings), the code runs in the emulator and under
> Wine, but please run `examples\bin\*.exe` on a Windows machine and report any difference.

## About the original `test.adac`

That file is a *syntax catalogue* and deliberately contains constructs the language defines as errors, so
`adac` rejects it (class inside `main`, four identical `methodName()` methods, `for(_; i<=10…)` without a
declared `i`, `setMen(variable)` / `push_back` on an undeclared variable, undeclared `XException`/`YException`,
and `printf` with `%d…` but no arguments). `examples/test_original_english.adac` is that file translated to
English (still rejected, on purpose); `examples/test_fixed.adac` is the compilable version with every construct.

## Known limitations (roadmap)

* **Lambdas/closures** and nested functions do not exist yet.
* No generics for user classes (only the built-in `List<T>` and `Map<String,T>`); `Map` keys must be `String`.
* `List.indexOf/contains` compare the *word value* (for `List<String>` that is object identity, not the text).
* `instanceof` with an interface is not supported yet (use a class `instanceof` or a cast).
* Multi-dimensional arrays: only `new T[n][m]` (2 dimensions); an `enum` is an `int` with names (no type checking between enums).
* `import` is textual inclusion (no namespaces); `#definePackage` does not produce libraries yet.
* No overloading by type (only by number of parameters); no `switch` on `float`.
* Floats above ~9×10¹⁸ are not printed in scientific notation.
* Runtime errors (division by zero, `null`, bad index) end the program with a message; `catch` cannot catch them.
* The GC does not move objects and does not recognize pointers to the *middle* of a block held only in registers (generated code never does that).

## Benchmark (C × adaC × Python)

`benchmarks/bench.py` builds and runs 5 equivalent programs (fib(35), a 30M-iteration loop, a π series with
20M float terms, a 5M sieve, 3M objects) and compares them. adaC's `.exe` runs under **Wine** (native code,
no emulation). Results are in `benchmarks/RESULT.txt` (gcc 13, CPython 3.12, best of 3).
