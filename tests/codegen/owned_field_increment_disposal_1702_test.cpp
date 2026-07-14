// Codegen fix #1702 — `++`/`--` on an `owned` class-typed FIELD whose type
// overloads `operator ++`/`operator --` must apply §28.7 drop-on-overwrite:
// dispose the displaced previous value, exactly once (spec §17.6 / §28.7,
// v0.185).
//
// §17.6 (v0.185): "When `++` or `--` is applied to an `owned` field of class
// type T whose `operator ++` / `operator --` returns a new instance, the §28.7
// drop-on-overwrite rule applies: the operator result (an owned value) is
// produced first, then the displaced previous value is disposed implicitly,
// then the new value is stored — no explicit prior disposal and no warning is
// involved."
//
// The bug: the IncrementStmt operator-overload branch stored the operator result
// through the LOCAL disposal path (disposeDisplacedOwnedClassLocal), which is a
// no-op for a FIELD target — so the displaced field value was silently lost,
// leaking on every `_field++`. The plain-assignment path already routed field
// targets through lowerOwnedClassFieldStore (§28.7 load-old / null+identity-guard
// / dispose-old / store-new); the fix routes the ++/-- store the same way.
//
// SOUNDNESS: disposal must be exactly once. lowerOwnedClassFieldStore's identity
// guard (new==old → skip) and null guard (moved-from/never-assigned → skip)
// prevent a double-free; the operator borrowed the old value as its parameter, so
// disposing it after the call is safe. A non-owned target (an `int` field, an
// owned LOCAL, a borrow field) must be untouched by the field helper — the
// controls below lock that in.
//
// Oracle: Counter's ctor bumps static Counter.n (+1), its destructor decrements
// it (-1); after the owning scope ends, `Counter.n` is the disposed-exactly-once
// balance (0). runAotChecked instruments the generated code with ASan and runs
// under LSan, so a leak / double-free / use-after-free flips r.ran to false
// (-999) — the memory verdict rides the same assertion as the counter value.
//
// Spec citations: §17.6 (increment/decrement on an owned class field, v0.185),
// §28.7 (drop-on-overwrite, v0.161), §25 (operator overloading).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Compile with generated-code AddressSanitizer, run under LeakSanitizer, return
// the exit code. A leak / double-free / use-after-free FAILS the run (ran=false
// -> -999), so the sanitizer verdict rides the same assertion as the value.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

void expectDisposedAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
}

// Counter: ctor +1 / dtor -1; operator ++/-- return a fresh owned Counter.
const std::string kCounterHdr =
    "module M; "
    "class Counter "
    "  public static n: int := 0; "
    "  public Value: int; "
    "  public constructor(v: int) n := n + 1; Value := v; end constructor; "
    "  public static method operator ++(c: Counter): owned Counter => "
    "    new Counter(c.Value + 1); "
    "  public static method operator --(c: Counter): owned Counter => "
    "    new Counter(c.Value - 1); "
    "  destructor n := n - 1; end destructor; "
    "end Counter; ";

}  // namespace


// §17.6 / §28.7 / #1702: `_field++` on an owned class field disposes the
// displaced value exactly once. Box holds `owned _c: Counter` (ctor sets it);
// three `_c++` displace three previous Counters — the ctor's Counter(0), then
// Counter(1), then Counter(2). Without the fix each displaced value LEAKS (LSan
// fires → -999). With the fix each is disposed exactly once; only Counter(3) is
// live inside the scope (Counter.n == 1), and it is disposed when the Box's
// `using var` scope ends → Counter.n == 0.
BEZEL_TEST(owned_field_increment_1702,
           field_increment_disposes_displaced_spec_28_7_v0_185) {
    expectDisposedAndLeakFree(kCounterHdr +
        "class Box "
        "  public owned _c: Counter; "
        "  public constructor() _c := new Counter(0); end constructor; "
        "  public method Bump() _c++; end Bump; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  do using var b := new Box(); "
        "    b.Bump(); b.Bump(); b.Bump(); "
        "    if Counter.n <> 1 then return 101; end; "  // only Counter(3) live
        "  end; "                                       // Box disposes _c
        "  return Counter.n; "                          // 0
        "end Main; end C;", 0);
}

// §17.6 / §28.7 / #1702: the decrement form `_field--` behaves identically — the
// displaced value is disposed exactly once, leak-clean.
BEZEL_TEST(owned_field_increment_1702,
           field_decrement_disposes_displaced_spec_28_7_v0_185) {
    expectDisposedAndLeakFree(kCounterHdr +
        "class Box "
        "  public owned _c: Counter; "
        "  public constructor() _c := new Counter(10); end constructor; "
        "  public method Dec() _c--; end Dec; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  do using var b := new Box(); "
        "    b.Dec(); b.Dec(); "                         // Counter(10),(9) disposed
        "    if Counter.n <> 1 then return 101; end; "   // only Counter(8) live
        "  end; "
        "  return Counter.n; "                           // 0
        "end Main; end C;", 0);
}

// CONTROL (soundness): a NON-owned `int` field `_count++` must be untouched by
// the owned-field-store helper — plain numeric increment, no disposal machinery,
// no leak. Guards against the field helper mis-firing on a non-owned target.
BEZEL_TEST(owned_field_increment_1702,
           non_owned_int_field_increment_unchanged_spec_17_6) {
    expectDisposedAndLeakFree(
        "module M; "
        "class Box "
        "  public _count: int := 0; "
        "  public constructor() end constructor; "
        "  public method BumpCount() _count++; end BumpCount; "
        "  public method GetCount(): int => _count; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  b.BumpCount(); b.BumpCount(); b.BumpCount(); "
        "  return b.GetCount(); "                         // 3
        "end Main; end C;", 3);
}

// CONTROL (regression guard for the fallback path): an owned class LOCAL `c++`
// must still dispose the displaced value through disposeDisplacedOwnedClassLocal
// — lowerOwnedClassFieldStore returns false for a local, so the store falls
// through to the local path unchanged. Two `c++` displace Counter(0),(1); only
// Counter(2) is live (n == 1), then explicitly disposed. Leak-clean.
BEZEL_TEST(owned_field_increment_1702,
           owned_local_increment_still_disposes_spec_28_7) {
    expectDisposedAndLeakFree(kCounterHdr +
        "class C public static method Main(): int "
        "  do var c := new Counter(0); c++; c++; "
        "    var m := Counter.n; c.Dispose(); return m; "  // 1
        "  end; "
        "end Main; end C;", 1);
}
