// Codegen — issue #2503: a bare-`T` STRING INTERPOLATION hole (`$"{val}"`,
// `val: T where T: value`) substituted with a struct-typed record/choice
// produced a SILENT WRONG VALUE instead of #2479's diagnostic (for a
// non-auto-hashable choice) or the §35.7/§9.1 default rendering
// (for an auto-hashable choice / record).
//
// Root cause (traced mechanically — NOT the cause #2503's own text
// proposed): the issue text attributed the bug to `emitValueToString`'s
// Choice branch (LlvmBackend_String.cpp) being gated on
// `fieldTy->declaration != nullptr`. Tracing the repro with instrumented
// codegen (fprintf at `emitValueToString`'s entry and at the interpolation
// dispatch site) showed `emitValueToString` is NEVER CALLED for this
// shape at all: the specialized instantiation `C$F$spec$Holder` lowers
// `val: T` to the LLVM value's ACTUAL (monomorphized) struct type
// directly (`%choice.Holder = type { i32, {i8}, {ptr} }`), while
// `ctx_.exprType(*part.expr)` (the `rt` the interpolation dispatch reads)
// stays the bare, declaration-less `T` — sema never rewrites it. The
// string-interpolation dispatch (LlvmBackend_Exprs.cpp, `lowerExpr`'s
// `InterpolatedStringExpr` case) has two struct-handling gates: the
// LLVM-type-inference fallback matches only isPointerTy/isIntegerTy/
// isDoubleTy/isFloatTy (never isStructTy), and the dedicated
// "For struct (record value)" block requires `rt->declaration` truthy —
// which a bare T never has. NEITHER gate matches a struct-typed bare-T
// value, so `pv` stays null and `if (pv) appendPart(...)` never fires:
// the interpolation hole is not rendered as `""` — it is DROPPED from the
// accumulated string entirely (a distinct, more severe defect than the
// literal "renders as empty string" framing, though for a program whose
// only content is that one hole the two are externally indistinguishable
// — `$"{val}"` with nothing else evaluates to `""` either way).
//
// Fix: LlvmBackend_Exprs.cpp's "For struct" block now resolves a bare,
// declaration-less `rt` through the active `genericSubstSemaTy_`
// substitution first — the SAME resolution `emitValueToString`'s own
// bare-T redirect already performs (LlvmBackend_String.cpp) and the SAME
// resolution the direct-call bare-T redirect in `lowerCall` already
// performs (LlvmBackend_Calls2.cpp) — so a struct-typed T instantiated to
// a Record or Choice dispatches through the SAME "For struct" logic a
// concrete-typed operand already uses (the user-ToString-override lookup,
// `emitRecordToString`, `emitChoiceToString`, and — for a choice that is
// NOT auto-hashable (§35.7 defines no ToString default for that
// classification) — the pre-existing #2479/#2476 diagnostic). This closes
// the same "one enforcement point" gap #2503 asked for, inside the block
// that already held the concrete-case enforcement, rather than adding a
// fourth parallel copy.
//
// Both the Record and Choice arms of that block shared the identical
// `rt->declaration` gate, so both get the identical fix; a record-typed
// bare-T interpolation hole had the exact same silent-drop defect and is
// covered below as a same-root-cause regression guard, not scope creep.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must
// be distinguished by asserting on the diagnostic TEXT, never by exit code
// or `!r.ran` alone.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

const char* kPair =
    "choice Pair P(x: int, y: string); end Pair; ";
const char* kNonAutoEqBearing =
    "class Widget end Widget; "
    "choice Holder Empty, Filled(item: Widget); end Holder; ";
const char* kNonAeqDiag =
    "member `ToString` on a choice that is not auto-hashable";

}  // namespace

// ── The bug's own repro shape: bare-T interpolation, non-auto-equality-
//    bearing choice — §35.7 defines no ToString default for this
//    classification, so §18.5's "a hole without a format spec formats via
//    the value's ordinary ToString()" must surface the #2479 diagnostic,
//    never a silently-wrong value. FAILS before the fix (compiles clean,
//    `s == ""`, `Main` returns 1); PASSES after (compile-time Error). ────

BEZEL_TEST(generic_bare_typeparam_tostring_interp_2503,
           non_auto_equality_bearing_bare_t_interpolation_diagnosed) {
    // §35.3 (v0.334): `Holder.Filled(item: Widget)` is a PLAIN class-typed
    // payload slot, so its argument is a borrow, and §9.7's
    // no-temporary-source rule applies verbatim — `Holder.Filled(new
    // Widget())` is rejected before the program ever reaches the ToString
    // check this row exists to pin. The row's INTENT is the §35.7
    // diagnostic on the interpolation hole, so the source is bound to a
    // named local first — §35.3's own stated remedy ("bind the source to a
    // named local first"). Measured: with the temporary the program now
    // draws the §35.3 reject and never reaches §35.7; with the named local
    // it draws exactly the kNonAeqDiag / #2479 pair this row asserts, so the
    // rule under test is unchanged.
    const std::string program =
        std::string("module M; ") + kNonAutoEqBearing +
        "class C "
        "  public static method F<T>(val: T): string where T: value "
        "    return $\"{val}\"; "
        "  end F; "
        "  public static method Main(): int "
        "    var w := new Widget(); "
        "    var s := F(Holder.Filled(w)); "
        "    return (if s = \"\" then 1 else 0); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(hasError(program, kNonAeqDiag));
    BEZEL_EXPECT_TRUE(hasError(program, "#2479"));
}

// ── The auto-hashable counterpart: §35.7's real default
//    ("TypeName.CaseName(field1: value1, …)") must actually render through
//    the bare-T interpolation hole, not vanish. FAILS before the fix
//    (`s == ""`, hole silently dropped); PASSES after (correct §35.7
//    text). ──────────────────────────────────────────────────────────────

BEZEL_TEST(generic_bare_typeparam_tostring_interp_2503,
           auto_equality_bearing_bare_t_interpolation_renders) {
    const std::string program =
        std::string("module M; ") + kPair +
        "class C "
        "  public static method F<T>(val: T): string where T: value "
        "    return $\"got {val}\"; "
        "  end F; "
        "  public static method Main(): int "
        "    var s := F(Pair.P(3, \"hi\")); "
        "    return (if s = \"got Pair.P(x: 3, y: hi)\" then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// Payload-free case through the same path.
BEZEL_TEST(generic_bare_typeparam_tostring_interp_2503,
           auto_equality_bearing_bare_t_interpolation_payload_free_renders) {
    const std::string program =
        "module M; choice Color Red, Green(value: int); end Color; "
        "class C "
        "  public static method F<T>(val: T): string where T: value "
        "    return $\"{val}\"; "
        "  end F; "
        "  public static method Main(): int "
        "    var s := F(Color.Red); "
        "    return (if s = \"Color.Red\" then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── Same root cause, Record arm of the identical `rt->declaration`-gated
//    block (LlvmBackend_Exprs.cpp): a record-typed bare-T interpolation
//    hole had the exact same silent-drop defect. FAILS before the fix;
//    PASSES after — not scope creep, the same lines fix both arms. ──────

BEZEL_TEST(generic_bare_typeparam_tostring_interp_2503,
           record_bare_t_interpolation_renders) {
    const std::string program =
        "module M; record Pt(X: int, Y: int); "
        "class C "
        "  public static method F<T>(val: T): string where T: value "
        "    return $\"{val}\"; "
        "  end F; "
        "  public static method Main(): int "
        "    var s := F(new Pt(1, 2)); "
        "    return (if s = \"Pt(X: 1, Y: 2)\" then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── Regression guard: the CONCRETE (non-generic) interpolation case
//    (#2479's own fix) is untouched by this fix's added bare-T resolution
//    — `structRt` degrades to `rt` unchanged whenever `rt->declaration`
//    is already set. ────────────────────────────────────────────────────

BEZEL_TEST(generic_bare_typeparam_tostring_interp_2503,
           concrete_choice_interpolation_unaffected) {
    const std::string program =
        std::string("module M; ") + kPair +
        "class C public static method Main(): int "
        "  var p := Pair.P(3, \"hi\"); "
        "  var s := $\"got {p}\"; "
        "  return (if s = \"got Pair.P(x: 3, y: hi)\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

BEZEL_TEST(generic_bare_typeparam_tostring_interp_2503,
           concrete_non_auto_equality_bearing_interpolation_still_diagnosed) {
    const std::string program =
        std::string("module M; ") + kNonAutoEqBearing +
        "class C public static method Main(): int "
        "  using var w := new Widget(); "
        "  var h := Holder.Filled(w); "
        "  var s := $\"h={h}\"; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, kNonAeqDiag));
    BEZEL_EXPECT_TRUE(hasError(program, "#2479"));
}

// ── Leak-freedom: the fix routes a previously-dropped value through
//    `emitChoiceToString`/`emitRecordToString`, both fresh +1 producers —
//    confirm interpolation's existing `appendPart` release discipline
//    (LlvmBackend_Exprs.cpp) still consumes them exactly once on this now-
//    live path (mirrors #2479's own `_lsan_clean` loop idiom). ──────────

BEZEL_TEST(generic_bare_typeparam_tostring_interp_2503,
           bare_t_interpolation_choice_lsan_clean) {
    const std::string program =
        std::string("module M; ") + kPair +
        "class C "
        "  public static method F<T>(val: T): string where T: value "
        "    return $\"{val}\"; "
        "  end F; "
        "  public static method Main(): int "
        "    var count := 0; "
        "    var i := 0; "
        "    while i < 40 do "
        "      var s := F(Pair.P(i, \"z\")); "
        "      if s.Length > 0 then count := count + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(program);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}
