// fix #4243 (§26.1/§4.6, v0.298): an `extension string` computed read-only
// PROPERTY silently degraded to a zero/null value at its USE SITE, via the
// pre-existing #2491/#2434 "internal:" codegen backstop — including the
// spec's own §26.1 worked example (`IsBlank`).
//
// ROOT CAUSE, measured: sema accepted the access correctly (fix #702's
// extension-lookup fallback for a `string` receiver, ExprTypecheck_Calls.cpp
// ~L18619, resolves the member and types the MemberAccessExpr) — the defect
// is entirely in CODEGEN. `LlvmBackend_Exprs.cpp`'s CG27a block ("intrinsic
// string member access") runs BEFORE the generic computed/block-getter
// dispatch (CG33b) that every OTHER extension-property receiver (int, bool,
// record, class — see extension_property_primitive_test.cpp) already goes
// through, and — for ANY bare (no-parens) member access whose receiver's
// static type is the `string` primitive — recognised only `Length` and
// `IsAscii` before unconditionally `return nullptr`ing for every other name,
// on the (correct, when it was written) assumption that "sema would have
// flagged any unknown member already". Fix #702 made that assumption stale
// for exactly the extension-PROPERTY case (extension METHODS are called with
// parens — `s.M()` — and go through the unrelated `lowerCall` path, so they
// were never affected). MEASURED that the failure is receiver-type-keyed, not
// getter-body-keyed: even a getter that never reads `this` at all
// (`property Loud: string => "hi";`) degraded identically to one that does —
// proving the CG27a gate rejects before the getter body is ever reached, not
// after evaluating it.
//
// MEASURED that this is SPECIFIC to `string`, not the broader #4229
// null-declaration population: `int`/`bool`/record/class extension
// properties already work (extension_property_primitive_test.cpp), and are
// swept again below as an explicit regression control. `Array<int>` was also
// probed as a second, non-string builtin data point: an extension PROPERTY on
// it is REJECTED at compile time ("no member `IsPair` on `Array<T>`") rather
// than silently degrading — a different (out-of-scope, not touched here)
// behavior, not the #4243 defect class.
//
// THE FIX: gate the CG27a intercept to the two real string intrinsics
// (Length/IsAscii); for any other bare member name, check
// computedGetterOf/blockGetterOf (the same predicate the file's own
// degenerate-receiver `Length` fallback arm a few lines below already uses)
// before swallowing the access — a real extension property falls through
// unevaluated (its receiver is deliberately NOT lowered here, to avoid a
// double lowering / double refcount once CG33b lowers it itself) to the
// generic dispatch every other receiver kind already uses.
//
// Both directions are asserted for every boolean-returning row (§26.1's
// `IsBlank` is a `bool`): a degraded/constant answer would only ever produce
// ONE of {true, false}, so pinning both a blank and a non-blank input is what
// rules out a bare `true`/`false`/`0` constant satisfying the assertion by
// accident.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
namespace { int run(const std::string& p){ auto r=runAot(p); return r.ran?r.exitCode:-999; } }

// spec §26.1 (v0.298) worked example, VERBATIM (the exact block text, minus
// the driver `Main`):
//
//   extension string
//     public property IsBlank: bool => this.Length = 0 or this.Trim().Length = 0;
//     public method Words(): owned Array<string> => this.Split(' ');
//   end;
//
// Row 1: a BLANK receiver (all-whitespace) — IsBlank must be true.
BEZEL_TEST(extension_property_string_4243, spec_26_1_isblank_blank_string) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extension string "
        "  public property IsBlank: bool => this.Length = 0 or this.Trim().Length = 0; "
        "  public method Words(): owned Array<string> => this.Split(' '); "
        "end; "
        "class C public static method Main(): int "
        "  var s := \"   \"; "
        "  return (if s.IsBlank then 1 else 0); "
        "end Main; end C;"), 1);
}

// Row 2: a NON-blank receiver — IsBlank must be false. Paired with row 1 so a
// constant-true or constant-false degrade fails at least one row.
BEZEL_TEST(extension_property_string_4243, spec_26_1_isblank_nonblank_string) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extension string "
        "  public property IsBlank: bool => this.Length = 0 or this.Trim().Length = 0; "
        "  public method Words(): owned Array<string> => this.Split(' '); "
        "end; "
        "class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  return (if s.IsBlank then 1 else 0); "
        "end Main; end C;"), 0);
}

// Row 3 (control): the spec's OWN sibling extension METHOD `Words` from the
// identical verbatim block. Extension methods are called with parens and
// dispatch through `lowerCall`, never CG27a's bare-MemberAccessExpr arm this
// fix touches — this pins that the fix's file-scoped change did not disturb
// the method half of the exact same worked example.
BEZEL_TEST(extension_property_string_4243, spec_26_1_words_method_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extension string "
        "  public property IsBlank: bool => this.Length = 0 or this.Trim().Length = 0; "
        "  public method Words(): owned Array<string> => this.Split(' '); "
        "end; "
        "class C public static method Main(): int "
        "  var s := \"the quick fox\"; "
        "  using var w := s.Words(); "
        "  return w.Count; "
        "end Main; end C;"), 3);
}

// Row 4: getter body that NEVER reads `this` at all — proves the pre-fix
// failure was keyed on the RECEIVER's type (string), not on anything the
// getter body does. Pre-fix this SEGFAULTED (the #2434 zero-init backstop
// stored a null `bz_string*`, and `.Length` on it dereferenced null).
BEZEL_TEST(extension_property_string_4243, const_body_ignores_receiver) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extension string public property Loud: string => \"hi\"; end; "
        "class C public static method Main(): int "
        "  var s := \"  \"; "
        "  var r := s.Loud; "
        "  return r.Length; "
        "end Main; end C;"), 2);
}

// Row 5: getter body that reads `this` via a trivial identity — the sibling
// shape the filed issue called out explicitly ("even `property Loud: string
// => this;` ... fails the same way once the result is used").
BEZEL_TEST(extension_property_string_4243, identity_body_reads_receiver) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extension string public property Loud: string => this; end; "
        "class C public static method Main(): int "
        "  var s := \"hi\"; "
        "  var r := s.Loud; "
        "  return r.Length; "
        "end Main; end C;"), 2);
}

// Row 6: accessed through a LOCAL (not chained directly at the use site) —
// axis: direct call vs. through a local.
BEZEL_TEST(extension_property_string_4243, via_local) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extension string "
        "  public property IsBlank: bool => this.Length = 0 or this.Trim().Length = 0; "
        "end; "
        "class C public static method Main(): int "
        "  var s := \"   \"; "
        "  var b := s.IsBlank; "
        "  return (if b then 1 else 0); "
        "end Main; end C;"), 1);
}

// Row 7: passed AS AN ARGUMENT to another call — axis: direct call vs.
// through a local vs. as an argument.
BEZEL_TEST(extension_property_string_4243, as_call_argument) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extension string "
        "  public property IsBlank: bool => this.Length = 0 or this.Trim().Length = 0; "
        "end; "
        "class C "
        "  public static method Pick(b: bool): int => (if b then 1 else 0); "
        "  public static method Main(): int "
        "    var s := \"   \"; "
        "    return C.Pick(s.IsBlank); "
        "  end Main; "
        "end C;"), 1);
}

// Row 8 (accept-side control): an ORDINARY (non-extension) string member —
// `.Length` — must be completely unaffected by this fix (it is still the ONE
// arm of the narrowed CG27a gate that runs unconditionally).
BEZEL_TEST(extension_property_string_4243, ordinary_length_control_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var s := \"  \"; "
        "  return s.Length; "
        "end Main; end C;"), 2);
}

// Row 9 (accept-side control): an extension METHOD on `string` (parens call,
// e.g. the pre-existing #702 lit/sema coverage) must keep working — this
// fix's gate only narrows the BARE (no-parens) MemberAccessExpr arm, which a
// method call never reaches.
BEZEL_TEST(extension_property_string_4243, extension_method_control_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extension string public method Shout(): string => this + \"!\"; end; "
        "class C public static method Main(): int "
        "  var s := \"hi\"; "
        "  return s.Shout().Length; "
        "end Main; end C;"), 3);
}
