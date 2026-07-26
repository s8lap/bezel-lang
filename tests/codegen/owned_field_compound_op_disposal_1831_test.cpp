// Codegen fix #1831 — compound assignment (`+=`, `-=`, `*=`, …) on an OWNED
// class-typed FIELD whose type overloads the corresponding binary operator
// must apply §28.7 drop-on-overwrite: dispose the displaced previous value,
// exactly once. Sibling of #1702 (which fixed the same defect for `++`/`--`).
//
// §25.1 / §17.6: a user-defined compound `target OP= value` desugars
// mechanically to `target := target.operator_op_(target, value)` (per §25.1
// "the compound forms desugar mechanically to read-modify-write using the
// underlying binary operator's overload"). The store back through the
// lvalue's normal assignment path is subject to §28.7 drop-on-overwrite
// for an owned field/local — a fresh owned operator result replaces the
// displaced previous value, and the displaced value must be disposed
// exactly once (no leak, no double-free, no skipped dispose).
//
// The bug: the AssignStmt user-defined-op branch stored the operator result
// through a plain `builder_.CreateStore(res, addr)`, never routing through
// `lowerOwnedClassFieldStore` (for fields) or `disposeDisplacedOwnedClassLocal`
// (for locals) — so an owned class field's displaced value was silently
// dropped, leaking on every `+=` invocation. The plain `:=` path already
// routed field targets through `lowerOwnedClassFieldStore`; the `++`/`--`
// path (the #1702 fix) routed through the same helper. The fix here routes
// the compound `OP=` store the same way.
//
// SOUNDNESS: disposal must be exactly once. lowerOwnedClassFieldStore's
// identity guard (new==old → skip) and null guard (moved-from/never-assigned
// → skip) prevent a double-free; the operator borrowed the old value as
// its parameter, so disposing it after the call is safe. A non-owned
// target (an `int` field, a borrow field, a record field) must be
// untouched by the field helper — the controls below lock that in.
//
// Oracle: Counter's ctor bumps static Counter.n (+1), its destructor
// decrements it (-1); after the owning scope ends, `Counter.n` is the
// disposed-exactly-once balance (0). runAotChecked instruments the
// generated code with ASan and runs under LSan, so a leak / double-free
// / use-after-free flips r.ran to false (-999) — the memory verdict
// rides the same assertion as the counter value.
//
// Spec citations: §25.1 (compound desugars to the underlying binary op),
// §28.7 (drop-on-overwrite), v0.185.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Compile with generated-code AddressSanitizer, run under LeakSanitizer,
// return the exit code. A leak / double-free / use-after-free FAILS the
// run (ran=false -> -999), so the sanitizer verdict rides the same
// assertion as the value.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// Counter: ctor +1 / dtor -1; `operator +(Counter, int)` returns a fresh
// owned Counter; the compound `_c += k` therefore desugars to
// `_c := _c.operator+(_c, k)` — a fresh owned instance replaces the
// old one. The displaced previous value must be disposed (the fix).
const std::string kCounterHdr =
    "module M; "
    "class Counter "
    "  public static n: int := 0; "
    "  public Value: int; "
    "  public constructor(v: int) n := n + 1; Value := v; end constructor; "
    "  public static method operator +(c: Counter, k: int): owned Counter => "
    "    new Counter(c.Value + k); "
    "  destructor n := n - 1; end destructor; "
    "end Counter; ";

}  // namespace


// §25.1 / §28.7 / #1831: `_c += k` on an owned class field disposes the
// displaced value exactly once. Two `+= 5` calls displace two Counters:
// the ctor's Counter(0) and the operator's Counter(5). Without the fix
// each displaced value LEAKS (LSan fires → -999). With the fix each is
// disposed exactly once; only Counter(10) is live inside the scope
// (Counter.n == 1), and it is disposed when Box's `using var` scope ends
// → Counter.n == 0.
BEZEL_TEST(owned_field_compound_op_1831,
           field_compound_add_disposes_displaced_spec_28_7_v0_185) {
    BEZEL_EXPECT_EQ(run(kCounterHdr +
        "class Box "
        "  public owned _c: Counter; "
        "  public constructor() _c := new Counter(0); end constructor; "
        "  public method Add(k: int) _c += k; end Add; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  do using var b := new Box(); "
        "    b.Add(5); b.Add(5); "
        "    if Counter.n <> 1 then return 101; end; "  // only Counter(10) live
        "  end; "                                       // Box disposes _c
        "  return Counter.n; "                          // 0
        "end Main; end C;"), 0);
}

// §25.1 / §28.7 / #1831: a `*= 2` (a non-`+` compound) follows the same
// path. The fix covers every user-defined binary op compound (the branch
// gates on ANY `opMethod` — not a specific token), so the dispose-old /
// store-new path is shared by `+=`, `-=`, `*=`, etc. Counter(1, *, 2, *)
// then disposed, only Counter(4) live.
BEZEL_TEST(owned_field_compound_op_1831,
           field_compound_mul_disposes_displaced_spec_28_7_v0_185) {
    const std::string src =
        "module M; "
        "class Counter "
        "  public static n: int := 0; "
        "  public Value: int; "
        "  public constructor(v: int) n := n + 1; Value := v; end constructor; "
        "  public static method operator *(c: Counter, k: int): owned Counter => "
        "    new Counter(c.Value * k); "
        "  destructor n := n - 1; end destructor; "
        "end Counter; "
        "class Box "
        "  public owned _c: Counter; "
        "  public constructor() _c := new Counter(1); end constructor; "
        "  public method Scale(k: int) _c *= k; end Scale; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  do using var b := new Box(); "
        "    b.Scale(2); b.Scale(2); "
        "    if Counter.n <> 1 then return 101; end; "  // only Counter(4) live
        "  end; "
        "  return Counter.n; "                          // 0
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// CONTROL (soundness): a NON-owned `int` field `_count += 5` must be
// untouched by the owned-field-store helper — plain numeric compound
// increment, no disposal machinery, no leak. Guards against the field
// helper mis-firing on a non-owned target.
BEZEL_TEST(owned_field_compound_op_1831,
           non_owned_int_field_compound_unchanged_spec_28_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box "
        "  public _count: int := 0; "
        "  public constructor() end constructor; "
        "  public method Bump(k: int) _count += k; end Bump; "
        "  public method GetCount(): int => _count; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  b.Bump(3); b.Bump(4); "
        "  return b.GetCount(); "                        // 7
        "end Main; end C;"), 7);
}

// CONTROL (regression guard for the fallback path): an owned class
// LOCAL `c += k` must still dispose the displaced value through
// disposeDisplacedOwnedClassLocal — lowerOwnedClassFieldStore returns
// false for a local, so the store falls through to the local path
// unchanged. Two `+= 5` displace Counter(0),(5); only Counter(10)
// is live (n == 1), then explicitly disposed. Leak-clean.
BEZEL_TEST(owned_field_compound_op_1831,
           owned_local_compound_still_disposes_spec_28_7) {
    BEZEL_EXPECT_EQ(run(kCounterHdr +
        "class C public static method Main(): int "
        "  do var c := new Counter(0); c += 5; c += 5; "
        "    var m := Counter.n; c.Dispose(); return m; "  // 1
        "  end; "
        "end Main; end C;"), 1);
}
