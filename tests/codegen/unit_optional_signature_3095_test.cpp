// Spec-derived regression test for issue #3095 — `()?` AT A SIGNATURE POSITION
// (language spec §4.7 "The unit type", v0.252; §21.7 "return"; §17.4).
//
// §4.7, verbatim (v0.252):
//
//   "`()` is the type with exactly one value, also written `()`. It is the
//    return type of methods declared without an explicit return type.
//
//    Wrapping the unit type with `?` (yielding `()?`) is permitted by the
//    grammar and produces an optional with two values, `null` and `()`. It is
//    rarely useful; the natural representation of "presence or absence" for a
//    procedure-shaped result is `bool`."
//
// §17.4 leans on the same sentence when it rejects the OPERATOR form: "`()?`
// remains a well-formed type (§4.7); the rejection is of the operator form, not
// of the type." That sentence was only half true in the implementation.
//
// ── WHAT IT REPLACES (measured on 663a68e, the parent) ─────────────────────
//
// (a) A `()?` PARAMETER **crashed the compiler**:
//
//       public static method Take(u: ()?): int … end Take;
//
//     `bezelc -c -o out.o p.bzl` → SIGILL, exit 132, core dumped, NO
//     diagnostic. Backtrace (gdb, same base):
//       DataLayout::getPrefTypeAlign  ←  IRBuilderBase::CreateAlloca
//                                     ←  Emitter::entryAlloca
//                                     ←  Emitter::bindParameters
//     `lowerTypeRef`'s `Kind::Unit` arm answered LLVM `void` REGARDLESS of
//     `nullable`; `void` has no size or alignment, and this libLLVM build
//     compiles the `DataLayout` `llvm_unreachable` to a hardware trap. #1619
//     had already met this at a unit LOCAL and substituted a one-byte
//     placeholder there — but as a per-site convention, so the parameter slot
//     (which has no such guard) trapped.
//
// (a′) A plain `()` PARAMETER — `method Take(u: ())` — crashed IDENTICALLY on
//     the same base, and is covered here too: it is the same trap, and §14.2
//     lists "the unit type" among the `where T: value` substitutions, so the
//     position is reachable by construction and not merely by writing it out.
//     A LAMBDA parameter (§27.1) is a third site with the same defect — there
//     the trap is the `CreateStore` of the incoming argument into its slot
//     (`DataLayout::getABITypeAlign`), not the alloca — and is covered too.
//
// (b) A `()?` RETURN type was REJECTED as a callable with no return type:
//
//       public static method Nothing(): ()?
//         return null;                     ← "this callable has no return type
//       end Nothing;                          — `return` may not carry a value
//                                             (§21.7)"
//
//     sema's `returnsValue` tested `kind == Unit` without consulting
//     `nullable`, folding `()?` into §4.7's "It is the return type of methods
//     declared without an explicit return type" case — but `()?` is a
//     DIFFERENT type, with two values, so a method ascribed it does return one.
//
// ── THE REPRESENTATION THIS FIX COMMITS TO ────────────────────────────────
//
// §4.7 gives `()?` exactly two values, i.e. one BIT, so the tag alone carries
// the type. It joins the nullable-VALUE-type family (`isNullableUnitTy` under
// `isNullableValueTy`) and lowers to the family's tagged `{i8 hasValue, i8}`
// struct, whose payload byte is INERT — §4.7 gives the unit type exactly one
// value, so no byte pattern distinguishes one `()` from another. That is a
// tag-only carrier in content; keeping the family's two-element SHAPE is what
// makes every existing element-1 read well-typed for `()?` without a per-site
// arm to forget.
//
// ── ACCEPT-CONTROLS ────────────────────────────────────────────────────────
//
// The controls that matter are that the NON-nullable `()` still behaves exactly
// as it did everywhere it already worked: a unit RETURN type is still LLVM
// `void` (a `: ()` method with a bare `return;` still compiles and runs), a
// unit LOCAL still works (#1619), and `var o: Object := ();` still boxes
// (#2915). Those are asserted below alongside the fixed shapes.
//
// ── WHERE THE "NO `internal:` RESIDUAL" ORACLE LIVES ──────────────────────
//
// NOT here. `runAot` populates `JitResult::diagnostics` only on a compile
// REJECT (or a compiler crash/timeout) — a Warning emitted by a program that
// COMPILES never reaches it, so a `hasInternalNote`-style assertion in this
// file would be structurally incapable of failing. The silence oracle is
// tests/lit/unit_optional_signature_accept_3095.bzl, which pipes the real
// `bezelc` stderr through FileCheck.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool hasErrorText(const bezel::test::JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ── (a) THE CRASH: a `()?` PARAMETER ───────────────────────────────────────

BEZEL_TEST(unit_optional_signature_3095,
           a_nullable_unit_parameter_compiles_and_discriminates_spec_4_7) {
    // The issue's own program, extended so the ANSWER is observable and not
    // merely the absence of a crash: §4.7's two values must arrive at the
    // callee distinct from one another.
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Take(u: ()?): int "
        "    if u = null then return 1; end; "
        "    return 2; "
        "  end Take; "
        "  public static method Main(): int "
        "    var n: ()? := null; "
        "    var s: ()? := (); "
        "    return C.Take(n) * 10 + C.Take(s); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(12, r.exitCode);           // null → 1, present → 2
}

BEZEL_TEST(unit_optional_signature_3095,
           the_nullable_unit_literals_pass_directly_as_arguments_spec_4_7) {
    // The same two values written AT the call site rather than through a local
    // — the argument-position half of the coercion (`null` → the all-zero
    // optional; `()` → the present `{1, 0}`).
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Take(u: ()?): int "
        "    if u = null then return 1; end; "
        "    return 2; "
        "  end Take; "
        "  public static method Main(): int "
        "    return C.Take(null) * 10 + C.Take(()); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(12, r.exitCode);
}

// ── (a′) THE SAME CRASH AT A PLAIN `()` PARAMETER ──────────────────────────

BEZEL_TEST(unit_optional_signature_3095,
           a_plain_unit_parameter_compiles_and_runs_spec_4_7) {
    // Not `()?` — the NON-nullable unit type at the same position, which
    // trapped identically (SIGILL, exit 132) on 663a68e. §4.7's unit type
    // carries no information, so the parameter's only job is to exist; the
    // observable is that the call happens and the callee's body runs.
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Take(u: ()): int "
        "    return 7; "
        "  end Take; "
        "  public static method Main(): int "
        "    return C.Take(()); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(7, r.exitCode);
}

BEZEL_TEST(unit_optional_signature_3095,
           a_unit_typed_lambda_parameter_compiles_spec_4_7_27_1) {
    // A THIRD site with the same trap, distinct from `bindParameters`':
    // `lowerLambda` binds each parameter by STORING the incoming argument into
    // its slot, and a `void` parameter type made that store trap
    // (`DataLayout::getABITypeAlign(void)` under `CreateStore`, SIGILL,
    // measured on this fix's own base — the `()?` form is covered by the two
    // calls below, the `()` form by the declaration alone).
    //
    // ⚠ The `()`-parameter closure is DECLARED and not CALLED on purpose. The
    // function-VALUE call path builds its arguments in its own loop, which does
    // not yet supply the unit operand — `lu(())` degrades with a located #3079
    // note (filed separately). The property this test owns is that neither
    // DECLARATION traps the compiler any more, and that the `()?` closure
    // answers both of §4.7's values.
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var lu: (()) -> int := lambda (u: ()) => 6; "
        "    var lo: (()?) -> int := lambda (u: ()?) => do "
        "      if u = null then return 1; end; "
        "      return 2; "
        "    end; "
        "    var v: ()? := (); "
        "    return lo(null) * 10 + lo(v); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(12, r.exitCode);
}

// ── (b) THE FALSE REJECT: a `()?` RETURN TYPE ──────────────────────────────

BEZEL_TEST(unit_optional_signature_3095,
           a_nullable_unit_return_type_is_accepted_and_carries_both_values_spec_4_7_21_7) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Nul(): ()? "
        "    return null; "
        "  end Nul; "
        "  public static method Some(): ()? "
        "    return (); "
        "  end Some; "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    if C.Nul() = null then r := r + 1; end; "
        "    if C.Some() <> null then r := r + 2; end; "
        "    var x: ()? := C.Some(); "
        "    if x <> null then r := r + 4; end; "
        "    var y: ()? := C.Nul(); "
        "    if y = null then r := r + 8; end; "
        "    return r; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(15, r.exitCode);
    // The §21.7 diagnostic that used to fire on this program must be gone.
    BEZEL_EXPECT_FALSE(
        hasErrorText(r, "this callable has no return type"));
}

BEZEL_TEST(unit_optional_signature_3095,
           a_bare_return_in_a_nullable_unit_method_is_rejected_spec_21_7) {
    // The other side of the same §21.7 rule, and the control that proves the
    // context really changed: a `()?`-returning callable HAS a return type, so
    // `return;` must now be the rejected form. Pre-fix it was the ACCEPTED one.
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Nothing(): ()? "
        "    return; "
        "  end Nothing; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(
        hasErrorText(r, "this callable has a return type"));
    BEZEL_EXPECT_TRUE(hasErrorText(r, "\xC2\xA7""21.7"));
}

// ── LOCALS AND ASSIGNMENT: the two values must stay distinct ───────────────

BEZEL_TEST(unit_optional_signature_3095,
           assigning_the_unit_value_to_a_nullable_unit_local_stores_it_spec_4_7) {
    // MEASURED on 663a68e: `u := ();` emitted `internal: assignment RHS
    // expression failed to lower to a value … (store dropped — target retains
    // its previous value) — #2961` and left `u` reading as `null`. §4.7's two
    // values were collapsed into one — a WRONG VALUE, not a benign no-op.
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var u: ()? := null; "
        "    var r: int := 0; "
        "    if u = null then r := r + 1; end; "
        "    u := (); "
        "    if u <> null then r := r + 2; end; "
        "    u := null; "
        "    if u = null then r := r + 4; end; "
        "    return r; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(7, r.exitCode);
}

BEZEL_TEST(unit_optional_signature_3095,
           the_local_initializer_forms_still_answer_correctly_spec_4_7) {
    // The shapes the issue reports as ALREADY working — pinned so the
    // representation change cannot regress them.
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var u: ()? := null; "
        "    var v: ()? := (); "
        "    var r: int := 0; "
        "    if u = null then r := r + 1; end; "
        "    if v <> null then r := r + 2; end; "
        "    if u <> null then r := r + 4; end; "
        "    if v = null then r := r + 8; end; "
        "    return r; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(3, r.exitCode);
}

// ── ACCEPT-CONTROLS: the NON-nullable `()` is untouched ────────────────────

BEZEL_TEST(unit_optional_signature_3095,
           a_plain_unit_return_type_is_still_a_void_method_spec_4_7_21_7) {
    // §4.7: `()` "is the return type of methods declared without an explicit
    // return type", so an explicit `: ()` ascription is the SAME void-method
    // context and a bare `return;` is its correct form. This is exactly the
    // case `returnsValue`'s unchanged arm still claims.
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Nothing(): () "
        "    return; "
        "  end Nothing; "
        "  public static method Implicit() "
        "    return; "
        "  end Implicit; "
        "  public static method Main(): int "
        "    C.Nothing(); "
        "    C.Implicit(); "
        "    return 5; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(5, r.exitCode);
}

BEZEL_TEST(unit_optional_signature_3095,
           a_value_carrying_return_from_a_plain_unit_method_is_still_rejected_spec_21_7) {
    // The §21.7 rule the `!r->nullable` conjunct preserves: `: ()` is still a
    // no-return-type context, so `return ();` from it is still an error. If the
    // conjunct were dropped instead of added, this would silently start
    // compiling.
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Nothing(): () "
        "    return (); "
        "  end Nothing; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasErrorText(r, "this callable has no return type"));
    BEZEL_EXPECT_TRUE(hasErrorText(r, "\xC2\xA7""21.7"));
}

BEZEL_TEST(unit_optional_signature_3095,
           a_plain_unit_local_still_works_and_boxes_to_object_spec_4_7_8_6) {
    // #1619's unit local and #2915's unit box — the two positions the
    // non-nullable `()` already had, unchanged by the `()?` representation.
    // §4.7: "`var o: Object := ();` is grammatically valid and produces a boxed
    // `()`"; §8.5 renders that box as "()".
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var u: () := (); "
        "    var o: Object := (); "
        "    Console.WriteLine(o.ToString()); "
        "    if o is () then return 9; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(9, r.exitCode);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("()") != std::string::npos);
}

BEZEL_TEST(unit_optional_signature_3095,
           an_ordinary_nullable_value_type_parameter_is_unchanged_spec_7_2) {
    // The anti-over-reach control for the `isNullableValueTy` extension: the
    // family it joins must keep behaving exactly as before at the same
    // positions. `int?` is the family's canonical member.
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Take(v: int?): int "
        "    if v = null then return 1; end; "
        "    return v! + 1; "
        "  end Take; "
        "  public static method Give(b: bool): int? "
        "    if b then return 41; end; "
        "    return null; "
        "  end Give; "
        "  public static method Main(): int "
        "    return C.Take(C.Give(true)) * 2 + C.Take(C.Give(false)); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(85, r.exitCode);           // (42 * 2) + 1
}
