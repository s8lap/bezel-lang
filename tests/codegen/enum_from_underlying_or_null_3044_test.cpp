// fix #3044 — the branch-shaped tier beside §5.4's checked cast: the
// compiler-provided per-enum static member `FromUnderlyingOrNull` (proposal
// 0078, graduated spec v0.242).
//
// §10.4 (v0.242): "Every enum `E` with underlying type `U` carries one
// **compiler-provided** static member — no declaration is written, and no user
// declaration may override or overload it … `public static method
// FromUnderlyingOrNull(value: U): E?;` … It returns the declared constant whose
// underlying value equals `value`, or `null` when there is none. It is
// **total** — it never raises." Stdlib §9.9 documents the same shape as the §1
// `*OrNull` tier: "`E(n)` is the assert-shaped form (an undeclared value
// faults), `E.FromUnderlyingOrNull(n)` the branch-shaped form for boundary
// code, composing directly with `??`/`?.`."
//
// The user-declaration reject and the wider-argument reject are the lit family
// `from_underlying_or_null_3044__*`; the cross-module arm is
// tests/module/xmod_enum_checked_cast_3044_test.cpp.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
// 0, 1, 2 by §10.1 auto-increment.
const char* kPriority = "module M; enum Priority Low, Medium, High; end Priority; ";
}  // namespace

// ---------------------------------------------------------------------------
// §10.4 / stdlib §9.9 — the branch tier `E.FromUnderlyingOrNull(value): E?`
// ---------------------------------------------------------------------------

// "returns the declared constant whose underlying value equals `value`" — the
// hit case, unwrapped with `!` (§17.12).
BEZEL_TEST(enum_from_underlying_or_null_3044, hit_spec_10_4) {
    BEZEL_EXPECT_EQ(run(std::string(kPriority) +
        "class C public static method Main(): int "
        "  var n: int := 2; var p := Priority.FromUnderlyingOrNull(n); "
        "  return int(p!); "
        "end Main; end C;"), 2);
}

// "… or `null` when there is none. It is **total** — it never raises." The miss
// case must return null rather than fault.
BEZEL_TEST(enum_from_underlying_or_null_3044, miss_is_null_spec_10_4) {
    BEZEL_EXPECT_EQ(run(std::string(kPriority) +
        "class C public static method Main(): int "
        "  try var n: int := 9; var p := Priority.FromUnderlyingOrNull(n); "
        "      return (if p = null then 42 else 1); "
        "  catch OverflowException => return 2; end; "
        "end Main; end C;"), 42);
}

// §10.4's own boundary idiom: "tolerate with a default" —
// `Priority.FromUnderlyingOrNull(wire) ?? Priority.Low`.
BEZEL_TEST(enum_from_underlying_or_null_3044, coalesce_default_spec_10_4) {
    BEZEL_EXPECT_EQ(run(std::string(kPriority) +
        "class C public static method Main(): int "
        "  var bad: int := 9; var good: int := 1; "
        "  var a := Priority.FromUnderlyingOrNull(bad) ?? Priority.High; "
        "  var b := Priority.FromUnderlyingOrNull(good) ?? Priority.High; "
        "  return int(a) * 10 + int(b); "                       // High=2, Medium=1
        "end Main; end C;"), 21);
}

// "Under duplicate constant values (§10.2) the result is the shared value (all
// aliases denote it)."
BEZEL_TEST(enum_from_underlying_or_null_3044, duplicate_alias_spec_10_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum E Legacy := 1, Current := 1; end E; "
        "class C public static method Main(): int "
        "  var n: int := 1; var e := E.FromUnderlyingOrNull(n); "
        "  return int(e = E.Legacy) + int(e = E.Current) + int(e!); "  // 1+1+1
        "end Main; end C;"), 3);
}

// §10.3 / stdlib §9.9: "The parameter type is the underlying type `U` exactly."
// A byte-backed enum takes a `byte`; the miss still returns null.
BEZEL_TEST(enum_from_underlying_or_null_3044, byte_backed_spec_10_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Color: byte Red := 0, Green := 1, Blue := 2; end Color; "
        "class C public static method Main(): int "
        "  var hit: byte := 2; var miss: byte := 200; "
        "  var a := Color.FromUnderlyingOrNull(hit); "
        "  var b := Color.FromUnderlyingOrNull(miss); "
        "  return int(a!) * 10 + int(b = null); "                       // 21
        "end Main; end C;"), 21);
}

// A sparse declared set: the member answers correctly on both a gap value and a
// declared one (the OR-chain arm of the shared membership test).
BEZEL_TEST(enum_from_underlying_or_null_3044, sparse_set_spec_10_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum HttpStatus Ok := 20, NotFound := 44; end HttpStatus; "
        "class C public static method Main(): int "
        "  var gap: int := 30; var hit: int := 44; "
        "  var a := HttpStatus.FromUnderlyingOrNull(gap); "
        "  var b := HttpStatus.FromUnderlyingOrNull(hit); "
        "  return int(a = null) + int(b!); "                            // 1+44
        "end Main; end C;"), 45);
}
