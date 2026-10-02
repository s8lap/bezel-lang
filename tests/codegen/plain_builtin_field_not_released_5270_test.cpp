// fix #5270 (§28.4 "generated Dispose" step 2 / §28.5, v0.313): only an
// `owned` field is disposed with its container. emitOwnedFieldDisposals' arm
// for a vtable-less builtin field type (Stopwatch, Mutex,
// CancellationTokenSource, ...) never checked the field's `owned` modifier, so
// a PLAIN field of such a type was released by the container's disposer. That
// field is the §28.3 back-reference idiom: a plain constructor parameter stored
// into a plain class-typed field, a borrow whose owner disposes it. The second
// release was a double free. The generic form (`class Box<T> public v: T;` at
// T = Stopwatch) is #5242's literal repro; it became reachable once #5242 laid
// the instantiation out specialized.
//
// Oracle: runAotChecked (ASan + LSan on the generated program), 200x loops.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── The issue's repro, non-generic: `public v: Stopwatch` holding a borrow of a
// `using var`.
BEZEL_TEST(plain_builtin_field_not_released_5270, plain_stopwatch_field_borrow_not_released) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "import System.Diagnostics; "
        "class H "
        "  public v: Stopwatch; "
        "  public constructor(x: Stopwatch) v := x; end constructor; "
        "end H; "
        "class P "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 0..199 do "
        "      using var s := Stopwatch.StartNew(); "
        "      using var b := new H(s); "
        "      if b.v.IsRunning then n := n + 1; end; "
        "    end for; "
        "    if n = 200 then return 7; end; "
        "    return 1; "
        "  end Main; "
        "end P; "), 7);
}

// ── CONTROL, not a pin: two more vtable-less builtin field types (Mutex,
// CancellationTokenSource) as plain borrow fields. MEASURED passing on the
// pre-fix disposer too (their double release did not trip ASan here); kept so
// the `owned` gate is shown not to disturb them.
BEZEL_TEST(plain_builtin_field_not_released_5270, plain_mutex_and_cts_fields_borrow_not_released) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "import System.Threading; "
        "class H "
        "  public m: Mutex; "
        "  public c: CancellationTokenSource; "
        "  public constructor(a: Mutex, b: CancellationTokenSource) m := a; c := b; end constructor; "
        "end H; "
        "class P "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 0..199 do "
        "      using var mx := new Mutex(); "
        "      using var cts := new CancellationTokenSource(); "
        "      using var h := new H(mx, cts); "
        "      if not h.c.IsCancellationRequested then n := n + 1; end; "
        "    end for; "
        "    if n = 200 then return 7; end; "
        "    return 1; "
        "  end Main; "
        "end P; "), 7);
}

// ── CONTROL: an `owned` Stopwatch field is still released exactly once (LSan
// would report the 200 leaked stopwatches if the gate dropped it).
BEZEL_TEST(plain_builtin_field_not_released_5270, owned_stopwatch_field_initializer_still_released) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "import System.Diagnostics; "
        "class H "
        "  public owned v: Stopwatch := new Stopwatch(); "
        "end H; "
        "class P "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 0..199 do "
        "      using var b := new H(); "
        "      b.v.Start(); "
        "      if b.v.IsRunning then n := n + 1; end; "
        "    end for; "
        "    if n = 200 then return 7; end; "
        "    return 1; "
        "  end Main; "
        "end P; "), 7);
}

// ── #5242's literal repro (generic `v: T` at T = Stopwatch, a NON-owned field),
// looped: prints True once, never "bad", ASan/LSan clean.
BEZEL_TEST(plain_builtin_field_not_released_5270, generic_plain_t_field_at_stopwatch_issue_5242_repro) {
    auto r = runAotChecked(
        "module M; "
        "import System.Diagnostics as D; "
        "class Box<T> "
        "  public v: T; "
        "  public constructor(x: T) v := x; end constructor; "
        "end Box; "
        "class P "
        "  public static method Main() "
        "    for var i in 0..199 do "
        "      using var s := D.Stopwatch.StartNew(); "
        "      var b := new Box<Stopwatch>(s); "
        "      if not b.v.IsRunning then Console.WriteLine(\"bad\"); end; "
        "    end for; "
        "    Console.WriteLine(\"True\"); "
        "  end Main; "
        "end P; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n"));
}
