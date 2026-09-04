// Spec-derived regression tests for issue #3785 — the §28.14 method-arm
// binding gate, on the named-argument surface of the stdlib's owning-buffer
// and span families. The brief pins three rows:
//
//   T1 — OwningBuffer<int>(length := 4, init := lambda(...)) — the §28.14
//        call-bounded gate on the `init` parameter (plain function-typed
//        formal `init: (int) -> T` / `init: (int) -> owned T`) must NOT
//        false-reject when the args arrive NAMED in declaration order.
//
//   T2 — OwningBuffer<int>(4, lambda(...)) — POSITIONAL control, must
//        continue to admit identically (the §22.5-equivalent form).
//
//   T3 — Span<int>(Buffer := buf, Start := 0, Length := 4) — record-style
//        named args on `Span<T>`. Span<T>'s declaration is a record
//        `public record Span<T>(Buffer: OwningBuffer<T>, Start: int,
//        Length: int)`; a builtin constructor with reordered named args is
//        restricted per #3398/#3425, so the in-order named form is the
//        admissible spelling.
//
// ── WHY THIS IS A NO-OP ROW SUITE TODAY (REFUTED-FOR-NEW-WORK) ────────────
//
// The METHOD-arms fix landed at commit 55fa386 (issue #3785, b66): the
// METHOD arm of `scopedFormalAt` / `verifiedCallBoundedFormalAt` now reads
// the §22.5-BOUND formal via `boundFormalIndex(*args, argPos, formals)`,
// matching the constructor arm's #3779 shape. All five callers pass the
// argument list (it was already in hand from a surrounding loop). The
// constructor arm is reached for `new OwningBuffer<int>(...)`; the method
// arm is reached for `h.Keep(...)` (the B1/B2/B3/B4 pin in
// `method_named_arg_3785_test.cpp`).
//
// The OwningBuffer constructor's `init` parameter is plain (non-`scoped`,
// non-`owned`) and function-typed, so the relevant §28.14 gate is
// `verifiedCallBoundedFormalAt`: a `true` there SUPPRESSES the §28.14
// lambda-escape scan against the lambda passed at the bound slot. With the
// b66 fix in place, the gate's constructor arm reads the BOUND formal
// (slot 1 for `init`), so a `length := 4, init := lambda(...)` call is
// correctly admitted.
//
// ── WHAT THIS ROW SUITE PINS ────────────────────────────────────────────
//
// Three rows — one positive (named args on a constructor whose `init`
// callback carries a capture, the most §28.14-stressful shape), one
// positional control, and one record-constructor named-args control — all
// must draw the same verdict under the current build. This is the
// REFUTED-FOR-NEW-WORK regression guard: a future change that breaks
// the method-arm's bound-formal lookup against the owning-buffer / span
// family will turn one of these rows RED.

#include <string>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The capture-bearing `init` lambda is the §28.14-strict shape — a CAPTURE
// (`seed`) means `checkScopedAdmission` would normally reject a forwarding
// into a `scoped` position; the `init` parameter is plain (call-bounded),
// so the §28.14 gate SUPPRESSES the escape scan and the lambda is admitted.
// This is exactly the surface the §28.14 method-arm gate covers, so a
// future regression in bound-formal lookup would surface here.
const char* kBufClassT =
    "class Item "
    "  public v: int; "
    "  public constructor(v_: int) v := v_; end constructor; "
    "end Item; ";

// §28.14's lambda-escape diagnostic — the false-reject shape, the same
// pin `method_named_arg_3785_test.cpp` uses.
bool hasUnverifiableCaptures(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("captures of this function-typed expression cannot "
                           "be verified") != std::string::npos)
            return true;
    return false;
}

// §22.5 named-arg-binds-to-the-bound-formal diagnostic, the rejection a
// future regression in this fix would surface.
bool hasNoSuchParameter(const OwnershipResult& r,
                        const std::string& paramName) {
    const std::string needle = "no parameter named `" + paramName + "`";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// Built-in constructor named-args restriction (#3398/#3425): the gate
// rejects NAMED args on a built-in that does not carry parameter names.
// A future regression that lifts the restriction without binding the
// §22.5 form would surface here for `Span<T>` (a record, not a class).
bool hasBuiltInNoNamedArgs(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(
                "named arguments are not supported on this built-in member") !=
                std::string::npos ||
            d.message.find("the compiler carries no parameter names for it") !=
                std::string::npos)
            return true;
    return false;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// T1 — NAMED args on OwningBuffer<T> constructor (class T, capture-bearing
// `init` lambda). The §28.14 method-arm gate must read the BOUND formal
// for `init` (slot 1) and admit the lambda. PRE-b66 this gate fell
// through (the Function-Value arm at `funcParams[argPos]`, where `argPos`
// is the SOURCE index 1 — correct by accident here, but the wrong authority
// for the bug class b66 fixed). The b66 fix hardens the lookup so T1
// cannot regress.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 / §28.14 (v0.286), issue #3785
BEZEL_TEST(named_args_method_arm_gate_3785,
           owning_buffer_ctor_named_args_admits_capturing_init_class_T) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kBufClassT +
        "class C "
        "  public static method Main(): int "
        "    var seed := 5; "
        "    var b := new OwningBuffer<Item>(length := 4, "
        "                                    init := lambda(i: int) "
        "                                             => new Item(i + seed)); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(hasUnverifiableCaptures(r));
    BEZEL_EXPECT_FALSE(hasNoSuchParameter(r, "length"));
    BEZEL_EXPECT_FALSE(hasNoSuchParameter(r, "init"));
}

// ═══════════════════════════════════════════════════════════════════════════
// T2 — POSITIONAL control. §22.5 makes T1 and T2 equivalent — "naming
// an argument changes which parameter it binds to, never when it runs" —
// so their observable verdicts must agree. PRE-b66 the positional form
// already worked (source pos == bound pos by construction); POST-b66 the
// named form matches it.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5, issue #3785
BEZEL_TEST(named_args_method_arm_gate_3785,
           owning_buffer_ctor_positional_control_admits_capturing_init_class_T) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kBufClassT +
        "class C "
        "  public static method Main(): int "
        "    var seed := 5; "
        "    var b := new OwningBuffer<Item>(4, "
        "                                    lambda(i: int) "
        "                                      => new Item(i + seed)); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(hasUnverifiableCaptures(r));
}

// ═══════════════════════════════════════════════════════════════════════════
// T3 — Span<T>(Buffer := buf, Start := 0, Length := 4) — record-style
// named args on `Span<T>`, in DECLARATION order. `Span<T>` is a builtin
// record (`public record Span<T>(Buffer, Start, Length)`); per #3425 a
// builtin does not carry the parameter names needed to support REORDERED
// named args, but IN-ORDER named args must admit like the positional
// form (T3's mirror below).
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 / §4.14 (stdlib §4.14 Span<T>), issue #3785
BEZEL_TEST(named_args_method_arm_gate_3785,
           span_record_named_args_in_order_admit_like_positional) {
    const char* src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(4); "
        "    var s := new Span<int>(Buffer := buf, Start := 0, Length := 4); "
        "    return s.Length; "
        "  end Main; "
        "end C; ";
    auto r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(hasUnverifiableCaptures(r));
    // The #3425 in-order form must admit — it does NOT carry the
    // "named arguments are not supported" diagnostic, since the names
    // ARE in declaration order.
    BEZEL_EXPECT_FALSE(hasBuiltInNoNamedArgs(r));
    // Positional mirror, must draw the same verdict.
    auto posR = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(4); "
        "    var s := new Span<int>(buf, 0, 4); "
        "    return s.Length; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(hasUnverifiableCaptures(posR));
    BEZEL_EXPECT_FALSE(hasBuiltInNoNamedArgs(posR));
}
