// Codegen — issue #7803: a CLOSURE LOCAL passed to the BUILT-IN console's
// `Write(Line)` never released its closure cell.
//
// THE DEFECT. `var f: (int) -> int := lambda(x: int) => x + 1;`
// `Console.Out.WriteLine(f);` leaked the 64-byte `bz.closure` cell — the
// function value's heap object — on EVERY execution. The console's `Object`
// parameter is PLAIN (stdlib §2.1: `public static method WriteLine(value:
// Object);`), so the position is a BORROW (§22.3: "the `owned` modifier is
// what transfers ownership") and the LOCAL keeps sole ownership of the
// producer's +1. §28.9's function-typed bullet (spec
// docs/Bezel_Language_Spec.md:8120) puts function values on the
// runtime-managed tier, whose "lifetime is managed by the runtime" — and the
// runtime's manager for a closure local is the compiler-inserted scope-end
// release of the cell.
//
// ROOT CAUSE (src/codegen/LlvmBackend_Setup.cpp, `ClosureEscapeWalker::expr`,
// the CallExpr arm). The escape walk that decides whether a function-typed
// local may release its cell walked EVERY call argument with
// `safeRead=false` — "a call ARGUMENT is an ESCAPE" — except for two narrow
// reliefs, neither of which covered a BUILT-IN callee:
//   * `closureArgBindsMethodOwnTypeParamBorrow` requires a resolved USER
//     `sema::MethodSymbol` for the callee; the built-in console has none.
//   * the `GetOrAdd` factory relaxation, which is the dictionary's own.
// The console is the ONLY built-in whose plain `Object` parameter had no
// relief, so `escaped = true`, the local never entered
// `closureReleaseEligible_`, `lowerLocalDecl`'s `d.closureRelease = true` was
// never registered, and the scope-end `bz_closure_release` was never emitted.
//
// THE IR IS THE PROOF (measured with BEZEL_DUMP_IR=1 on this file's rows):
// the repro program's `main` has ZERO `bz_closure_release` call sites, while
// the byte-identical program with the closure in CALLEE position
// (`Console.WriteLine(f(1))`) has TWO, and the one with a USER method's plain
// parameter (`Use(f)`) has TWO as well — the callee is the only difference.
//
// THE FIX is in the WALKER, not in the console lowering: a closure local read
// at the console's non-retaining value position is a CALL-BOUNDED BORROW, so
// the walk must classify it as a non-escape and the LOCAL's scope-end release
// must fire. The console lowering must NOT discharge the closure: its
// parameter is plain, so releasing at the console would free the cell while
// the local still points at it — a double free the moment the walker's
// relief lands, and a use-after-free on any later read of the local. The
// console's own discharge stays keyed on the ARGUMENT'S SHAPE (a forming
// expression nothing binds), so a named local, a field, a parameter or an
// alias is never discharged there.
//
// FAIL-WITHOUT / PASS-WITH, measured program-by-program on one build of each
// compiler (`bezelc --sanitize=address`, LeakSanitizer ON, 200-iteration
// loops so a one-shot can never be the oracle):
//
//   program                                            pre-fix      post-fix
//   Console.Out.WriteLine(f)   (the issue's repro)      64 B / 1    0 / 0
//   Console.WriteLine(f)       (the issue's spelling)   64 B / 1    0 / 0
//   the R37 d04_c.bzl repro, verbatim        12800 B / 200    0 / 0
//   the closure declared INSIDE the loop
//     (one cell per iteration)              12800 B / 200    0 / 0
//   Console.WriteLine(f); var g := f(1);    (read after)   64 B / 1    0 / 0
//   Console.WriteLine(f); twice              (two reads)   64 B / 1    0 / 0
//   Console.WriteLine(f(1));   (callee position)          0 / 0      0 / 0
//   Use(f);                    (user method's plain
//                               parameter)                 0 / 0      0 / 0
//   Console.WriteLine(b.F);    (a FIELD's closure)        0 / 0      0 / 0
//   a closure RETURNED from a method                       0 / 0      0 / 0
//   a closure CAPTURED by a lambda                         0 / 0      0 / 0
//   EventSource<int>.Add(f);   (a built-in that KEEPS
//                               the closure)               0 / 0      0 / 0
//   Array<int>.Sort(cmp);      (another built-in that
//                               takes a function — see the
//                               row's own note)  12800 B / 200  12800 B / 200
//
// EVERY row asserts its value through the CAPTURED STDOUT, never through an
// exit status. The leak rows use the ASan+LSan runner (`runAotChecked`), so
// `ran == false` IS the leak assertion; the control rows that are
// deliberately LSan-exempt use the ASan-only runner (the generated-code ASan,
// which is what catches a double free / use-after-free, stays armed).

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The pre-fix rows: the leak is the FAIL-WITHOUT oracle, so the runner's
// `ran` flag (false on a LeakSanitizer report) is the assertion, and the
// stdout carries the VALUE assertion.
std::string checkedOut(const std::string& prog, bool& ran) {
    auto r = runAotChecked(prog);
    ran = r.ran;
    return r.ran ? r.stdoutText : std::string("<leak-or-asan>");
}

// The one row whose program leaks on BOTH the pre-fix and the post-fix
// compiler (a DIFFERENT built-in, and a pre-existing sibling of this issue —
// see that row's own note): LSan OFF, generated-code ASan ON, so the row
// asserts "no crash / no double free / right value" and not "leak-clean".
std::string lenientOut(const std::string& prog, bool& ran) {
    auto r = runAotCheckedLenient(prog);
    ran = r.ran;
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t pos = hay.find(needle);
         pos != std::string::npos;
         pos = hay.find(needle, pos + needle.size()))
        ++n;
    return n;
}

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() &&
           s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

const std::string kPre =
    "module M; "
    "class P public static method Main(): int ";

const std::string kPost = " return 0; end Main; end P;";

// The closure's §8.5 rendering — a function value prints its type
// (the console's own `ToString` route), so the console's output line for a
// `(int) -> int` value is exactly this.
const char* kFnLine = "(int) -> int";

}  // namespace

// ── THE ISSUE'S REPRO, `Console.Out.WriteLine(f)` spelling. Pre-fix: the
// 64-byte `bz.closure` cell leaks (LSan: "64 byte(s) leaked in 1
// allocation(s)"). Post-fix: clean, and the console still renders the
// function value 200 times (the value assertion, through stdout).
// (issue #7803; spec §28.9's function-typed bullet, stdlib §2.1's plain
// `value: Object` parameter)
BEZEL_TEST(console_closure_local_release_7803,
           console_out_write_line_closure_local_released) {
    bool ran = false;
    auto s = checkedOut(
        std::string(kPre) +
            "var f: (int) -> int := lambda(x: int) => x + 1; "
            "for var i in 0..<200 do "
            "  Console.Out.WriteLine(f); "
            "end for; "
            "Console.WriteLine(\"DONE\");" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);   // LeakSanitizer-clean: the FAIL-WITHOUT oracle
    BEZEL_EXPECT_EQ(countOccurrences(s, kFnLine), 200);
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}

// ── The issue's own `Console.WriteLine(f)` spelling — the same relief must
// cover the direct console call, not only the `Console.Out` facet.
BEZEL_TEST(console_closure_local_release_7803,
           console_write_line_closure_local_released) {
    bool ran = false;
    auto s = checkedOut(
        std::string(kPre) +
            "var f: (int) -> int := lambda(x: int) => x + 1; "
            "for var i in 0..<200 do "
            "  Console.WriteLine(f); "
            "end for; "
            "Console.WriteLine(\"DONE\");" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(countOccurrences(s, kFnLine), 200);
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}

// ── The issue's filed repro, the R37 C2-32 review's `d04_c.bzl`, VERBATIM
// (its extra declarations included — the closure local, the 200-iteration
// loop and the `Console.Out.WriteLine(f)` call). Pre-fix: "64 byte(s) leaked
// in 1 allocation(s)".
BEZEL_TEST(console_closure_local_release_7803,
           issue_repro_d04_c_verbatim_released) {
    bool ran = false;
    auto s = checkedOut(
        "module M; "
        "class Cls public constructor() end constructor; "
        "  public override method ToString(): string => \"cls\"; end Cls; "
        "class Res public constructor() end constructor; destructor end destructor; "
        "  public override method ToString(): string => \"res\"; end Res; "
        "record Rec(x: int); "
        "enum Color Red, Green; end Color; "
        "choice VC A(x: int), B; end VC; "
        "class P "
        "  static method Mk(i: int): string => \"mk\" + i.ToString(); "
        "  static method MkObj(i: int): Object => i; "
        "  public static method Main() "
        "    var f: (int) -> int := lambda(x: int) => x + 1; "
        "    for var i in 0..<200 do "
        "      Console.Out.WriteLine(f); "
        "    end for; "
        "  end Main; "
        "end P;", ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(countOccurrences(s, kFnLine), 200);
}

// ── The closure declared INSIDE the loop body — one cell per iteration, so
// the pre-fix leak is 12800 B / 200 allocations and the post-fix release
// fires at each iteration's scope end (not only at the function's).
BEZEL_TEST(console_closure_local_release_7803,
           closure_local_inside_loop_body_released_each_iteration) {
    bool ran = false;
    auto s = checkedOut(
        std::string(kPre) +
            "for var i in 0..<200 do "
            "  var f: (int) -> int := lambda(x: int) => x + 1; "
            "  Console.Out.WriteLine(f); "
            "end for; "
            "Console.WriteLine(\"DONE\");" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(countOccurrences(s, kFnLine), 200);
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}

// ── THE ROW THAT REJECTS A WRONG FIX: the closure is read AFTER the console
// call. The local still owns the cell, so the value must still be 2 — a fix
// that discharged the closure AT the console would print garbage or abort
// with a heap-use-after-free here. Pre-fix this row also leaks the cell
// (64 B / 1 allocation), so it is a repro row AND the no-UAF control.
BEZEL_TEST(console_closure_local_release_7803,
           read_after_console_call_no_use_after_free) {
    bool ran = false;
    auto s = checkedOut(
        std::string(kPre) +
            "var f: (int) -> int := lambda(x: int) => x + 1; "
            "for var i in 0..<200 do "
            "  Console.WriteLine(f); "
            "  var g := f(1); "
            "  Console.WriteLine(g); "
            "end for; "
            "Console.WriteLine(\"DONE\");" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(countOccurrences(s, kFnLine), 200);
    // every `f(1)` must still read a live cell and produce 2
    BEZEL_EXPECT_EQ(countOccurrences(s, "2\n"), 200);
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}

// ── The closure passed to the console TWICE: two reads of one cell, one
// release. A double release here would be a hard ASan double-free.
BEZEL_TEST(console_closure_local_release_7803,
           console_called_twice_no_double_free) {
    bool ran = false;
    auto s = checkedOut(
        std::string(kPre) +
            "var f: (int) -> int := lambda(x: int) => x + 1; "
            "for var i in 0..<200 do "
            "  Console.WriteLine(f); "
            "  Console.WriteLine(f); "
            "end for; "
            "Console.WriteLine(\"DONE\");" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(countOccurrences(s, kFnLine), 400);
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}

// ── CONTROL — the CALLEE position (`Console.WriteLine(f(1))`) was already
// relieved (the #8058 `exprCallee` relaxation) and must stay exactly as it
// was: clean, and the value 2.
BEZEL_TEST(console_closure_local_release_7803,
           control_callee_position_unchanged) {
    bool ran = false;
    auto s = checkedOut(
        std::string(kPre) +
            "var f: (int) -> int := lambda(x: int) => x + 1; "
            "for var i in 0..<200 do "
            "  Console.WriteLine(f(1)); "
            "end for; "
            "Console.WriteLine(\"DONE\");" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(countOccurrences(s, "2\n"), 200);
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}

// ── CONTROL — a USER method's plain function-typed parameter (`Use(f)`).
// That position already had a relief (the #5129/#5611 `ms` path), so it was
// already clean and must stay clean and UNCHANGED.
BEZEL_TEST(console_closure_local_release_7803,
           control_user_method_plain_parameter_unchanged) {
    bool ran = false;
    auto s = checkedOut(
        "module M; "
        "class P "
        "  static method Use(f: (int) -> int): int => f(1); "
        "  public static method Main(): int "
        "    var f: (int) -> int := lambda(x: int) => x + 1; "
        "    var n := 0; "
        "    for var i in 0..<200 do "
        "      n := n + Use(f); "
        "    end for; "
        "    Console.WriteLine(n);" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_TRUE(endsWith(s, "400\n"));
}

// ── CONTROL — a closure stored in a FIELD and passed to the console: the
// FIELD's own ownership is unchanged (the walker's verdict for a member
// access is not the call-argument rule), so no double free and no leak.
BEZEL_TEST(console_closure_local_release_7803,
           control_field_owned_closure_unchanged) {
    bool ran = false;
    auto s = checkedOut(
        "module M; "
        "class Box public F: (int) -> int; "
        "  public constructor() F := lambda(x: int) => x + 1; end constructor; "
        "end Box; "
        "class P public static method Main(): int "
        "  var b := new Box(); "
        "  for var i in 0..<200 do "
        "    Console.WriteLine(b.F); "
        "  end for; "
        "  Console.WriteLine(\"DONE\");" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(countOccurrences(s, kFnLine), 200);
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}

// ── CONTROL — a closure RETURNED from a method: the escape walk's verdict
// for a RETURN must not change (the value leaves the callee's scope, so the
// callee must NOT release it).
BEZEL_TEST(console_closure_local_release_7803,
           control_returned_closure_unchanged) {
    bool ran = false;
    auto s = checkedOut(
        "module M; "
        "class P "
        "  static method Take(): (int) -> int "
        "    var f: (int) -> int := lambda(x: int) => x + 1; "
        "    return f; "
        "  end Take; "
        "  public static method Main(): int "
        "    for var i in 0..<200 do "
        "      var f: (int) -> int := Take(); "
        "      Console.WriteLine(f(1)); "
        "    end for; "
        "    Console.WriteLine(\"DONE\");" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(countOccurrences(s, "2\n"), 200);
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}

// ── CONTROL — a closure CAPTURED by a lambda: a capture is still an escape
// (the env outlives the producing scope), so the local must NOT be released
// at its scope end.
BEZEL_TEST(console_closure_local_release_7803,
           control_captured_closure_unchanged) {
    bool ran = false;
    auto s = checkedOut(
        std::string(kPre) +
            "for var i in 0..<200 do "
            "  var f: (int) -> int := lambda(x: int) => x + 1; "
            "  var g: () -> (int) -> int := lambda() => f; "
            "  var h: (int) -> int := g(); "
            "  Console.WriteLine(h(1)); "
            "end for; "
            "Console.WriteLine(\"DONE\");" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(countOccurrences(s, "2\n"), 200);
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}

// ── CONTROL — a built-in that KEEPS the closure it is handed:
// `EventSource<int>.Add(f)` (stdlib §6.5) stores the handler in the event
// source. The escape walk must still classify that position as an ESCAPE,
// so the local must NOT be released at its scope end (the event still holds
// the cell). The row asserts: no crash, no double free, and the program's
// own output — the walker's verdict for that position is unchanged.
BEZEL_TEST(console_closure_local_release_7803,
           control_builtin_that_keeps_the_closure_unchanged) {
    bool ran = false;
    auto s = checkedOut(
        std::string(kPre) +
            "for var i in 0..<200 do "
            "  var f := lambda(x: int) => (); "
            "  var e := new EventSource<int>(); "
            "  e.Add(f); "
            "end for; "
            "Console.WriteLine(\"DONE\");" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}

// ── CONTROL — ANOTHER built-in that takes a function-typed argument:
// `Array<int>.Sort(cmp)` (stdlib §4.4). The sort READS the comparer and
// never stores it, so this position is the same SHAPE of borrow as the
// console's — but it is a DIFFERENT built-in, and this issue's fix is
// deliberately NARROW (the console only). MEASURED on both compilers
// (`bezelc --sanitize=address`, LSan ON, 200 iterations):
//
//   var cmp: (int, int) -> int := lambda(x: int, y: int) => x - y;
//   var a: Array<int> := [3, 1, 2];
//   a.Sort(cmp);
//
//     →  12800 B / 200 allocations leaked, IDENTICALLY on the pre-fix and
//        the post-fix compiler.
//
// ⇒ the row is a SIBLING RESIDUAL, not a regression: this fix changed
// nothing for it (the leak is the same 12800 B / 200 allocations before and
// after). It is asserted with LSan OFF (the generated-code ASan — what
// catches a double free / use-after-free — stays armed), because the
// pre-existing leak is NOT this issue's to fix, and the console-only scope
// of this fix is what the row pins.
BEZEL_TEST(console_closure_local_release_7803,
           control_other_builtin_function_argument_unchanged) {
    bool ran = false;
    auto s = lenientOut(
        std::string(kPre) +
            "for var i in 0..<200 do "
            "  var cmp: (int, int) -> int := lambda(x: int, y: int) => x - y; "
            "  var a: Array<int> := [3, 1, 2]; "
            "  a.Sort(cmp); "
            "  Console.WriteLine(a[0]); "
            "end for; "
            "Console.WriteLine(\"DONE\");" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);   // no crash / no double free under ASan
    BEZEL_EXPECT_EQ(countOccurrences(s, "1\n"), 200);
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}
