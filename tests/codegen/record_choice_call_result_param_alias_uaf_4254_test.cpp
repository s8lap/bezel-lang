// fix #4254 (§9.7 "Records and disposal" / §28.9 "Runtime-managed types",
// v0.298): a `string` local passed as a plain (no-modifier) argument to a
// resolved method whose declared return type is a RECORD or a value-like
// CHOICE is a heap-use-after-free once that method embeds the argument into
// its returned value and the argument's own scope ends before the returned
// alias is read.
//
// ROOT CAUSE: `Emitter::walkCallArgs` (LlvmBackend_Setup.cpp)'s #1430
// relaxation treats a bare read of a tracked string local, passed in a plain
// parameter position of a resolved user/imported method, as a CALL-BOUNDED
// borrow — not an escape — so the local stays armed for its own scope-end
// `bz_string_release`. Its own SOUNDNESS comment claims this is safe because
// "every path by which [the callee] could extend the argument's lifetime
// mints its own +1: a field store retains ..., a return of the param mints
// ..., a closure capture retains ..., and an onward pass repeats this rule
// one level down" — four enumerated channels, claimed exhaustive.
//
// They are not. A callee whose declared return type is a RECORD (positional,
// no explicit constructor) or a value-like CHOICE can embed the borrowed
// parameter into a FRESH VALUE and return THAT, going through NEITHER
// channel: `buildRecordValue`'s positional fast path (LlvmBackend_Exprs2.cpp)
// stores a positional constructor argument into its field with a bare
// `CreateStore` — no `emitStringRetain` — and a choice case-constructor
// argument is the identical raw shape. So
// `Helper.MakeInner(s: string): Box<string> return new Box<string>(s); end`
// called as `var inner := Helper.MakeInner(s);` returns a value whose field
// ALIASES the caller's `s` with no independent +1 — but the #1430 relaxation
// still marks `s` non-escaping, so `s`'s own scope-end release frees the
// string while the returned alias survives.
//
// A flat, single-scope caller never OBSERVES the dangling alias (the read
// always precedes scope-end disposal there — disposal runs only after the
// return value is computed). The defect becomes an observable ASan
// heap-use-after-free once `s` (the string local) is scoped narrower than a
// read of the value that embeds it — e.g. `s` declared inside a `do ... end;`
// block, with the embedding record/choice value escaping the block via a
// wider-scoped local. Every UAF row below is exactly that shape, matching
// the issue's own contrast: "the inline form of the same value round-trips
// correctly" (no intermediate string local ever reaches the #1430 relaxation)
// while the helper-returned, var-bound form does not.
//
// MEASURED (this file's own FAIL-WITHOUT/PASS-WITH cycle, isolated worktree,
// this tree for both legs — `git diff` captured before reverting, `git
// checkout --` to strip the fix, rebuild bezelc, re-run under
// `scripts/agent_check.sh --probe --asan`, then `git apply` to restore):
//   - narrow_scope_helper_returned_record_string_param (non-generic,
//     depth-1 record `Str(V: string)`): FAIL-WITHOUT is a real
//     `AddressSanitizer: heap-use-after-free` (READ of size 8, freed in
//     `main`, previously allocated in `main` — a 32-byte `bz.string`
//     header). PASS-WITH: clean, exit 5 (`"hello".Length`).
//   - narrow_scope_helper_returned_nested_generic_record (the issue's own
//     `Box<Box<string>>` shape, depth 2): FAIL-WITHOUT is the identical
//     `AddressSanitizer: heap-use-after-free` signature. PASS-WITH: clean,
//     exit 5.
//   - narrow_scope_helper_returned_choice_string_payload (`Tag.Named(s)`):
//     FAIL-WITHOUT is the identical `AddressSanitizer: heap-use-after-free`
//     signature — confirming the CHOICE exclusion this fix adds is
//     load-bearing, not merely extra caution. PASS-WITH: clean, exit 5.
// So 3 of 3 UAF rows fail without the fix and pass with it (named above);
// every other row in this file is a control that is clean on BOTH sides
// (the fix narrows a relaxation, it does not change any accepted program's
// accepted-ness or computed value) and was not re-measured pre-fix — its
// purpose is to pin the CONTRAST and the axis boundary, not FAIL-WITHOUT.
//
// FIX: `walkCallArgs` now declines the #1430 relaxation for a plain string
// argument when the resolved callee's OWN declared return type (read via
// `ctx.resolvedType(*md->returnType)`, gated on `stringReleaseMode` so the
// dict-materializer / grouping / borrowing-Array walks that never had this
// relaxation's soundness argument are untouched) is a record or a value-like
// choice — the two type kinds whose fresh-construction fast paths do not
// retain a borrowed positional/payload argument. This is the safe direction
// (leak-over-UAF, §28.9's "how [runtime-managed reclamation happens] is not
// specified" leaves the possible extra leak within the spec's own silence);
// it does not add a retain to the record/choice construction fast paths,
// which would have to be proven not to double the +1 for the many argument
// shapes #2448/#2866/#3974 already established as producer-owned.
//
// A method returning a CLASS is unaffected and untested for FAIL-WITHOUT here
// — an explicit constructor's `this.f := x;` field store already retains a
// borrow via `lowerStringFieldStore`, so the #1430 four-channel argument is
// actually sound for that shape; the class control row below pins that the
// fix does not touch it.
//
// See tests/README.md for the `runAot`/`runAotChecked` harness this file
// uses exclusively.

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::checkedToolchainSkipped;
using bezel::test::runAotChecked;

namespace {
// Strict oracle: compiled, ran, exited cleanly, no ASan/LSan finding. `ran`
// is false on a UAF (which routes through an ASan hard-abort, fork-isolated)
// exactly as it is on a leak — see the #4232 test file's identical pattern.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ===========================================================================
// PRIMARY PIN 1/3 — the minimal, non-generic, depth-1 shape. No Box<T>
// nesting is required for the defect: the accounting bug is entirely in
// `s`'s own escape classification, independent of what record it ends up
// inside. FAIL-WITHOUT: AddressSanitizer heap-use-after-free (measured, see
// file header). PASS-WITH: exit 5 ("he"+"llo" = "hello", Length 5).
// ===========================================================================
BEZEL_TEST(record_choice_call_result_param_alias_uaf_4254,
           narrow_scope_helper_returned_record_string_param_no_longer_uaf) {
    const char* prog =
        "module M; record Str(V: string); "
        "class Helper "
        "  public static method Wrap(s: string): Str "
        "    return new Str(s); "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var outer: Str; "
        "    do "
        "      var s := \"he\" + \"llo\"; "
        "      outer := Helper.Wrap(s); "
        "    end; "
        "    return outer.V.Length; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 5);
}

// ===========================================================================
// PRIMARY PIN 2/3 — the issue's own shape verbatim: a GENERIC record
// (`Box<T>`), nested two levels deep (`Box<Box<string>>`), with the inner
// value produced by `Helper.MakeInner(s)` and bound to a `var` before being
// nested into the outer construction. FAIL-WITHOUT: identical ASan
// heap-use-after-free signature to PIN 1 (measured). PASS-WITH: exit 5.
// ===========================================================================
BEZEL_TEST(record_choice_call_result_param_alias_uaf_4254,
           narrow_scope_helper_returned_nested_generic_record_no_longer_uaf) {
    const char* prog =
        "module M; record Box<T>(Value: T); "
        "class Helper "
        "  public static method MakeInner(s: string): Box<string> "
        "    return new Box<string>(s); "
        "  end MakeInner; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var outer: Box<Box<string>>; "
        "    do "
        "      var s := \"he\" + \"llo\"; "
        "      var inner := Helper.MakeInner(s); "
        "      outer := new Box<Box<string>>(inner); "
        "    end; "
        "    return outer.Value.Value.Length; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 5);
}

// ===========================================================================
// PRIMARY PIN 3/3 — the CHOICE sibling of PIN 1: a value-like choice case
// constructor (`Tag.Named(s)`) is the identical raw-store shape as a
// positional record constructor. FAIL-WITHOUT: identical ASan
// heap-use-after-free signature (measured) — this is what makes the fix's
// Choice exclusion load-bearing, not merely defensive. PASS-WITH: exit 5.
// ===========================================================================
BEZEL_TEST(record_choice_call_result_param_alias_uaf_4254,
           narrow_scope_helper_returned_choice_string_payload_no_longer_uaf) {
    const char* prog =
        "module M; choice Tag Named(V: string); end Tag; "
        "class Helper "
        "  public static method Wrap(s: string): Tag "
        "    return Tag.Named(s); "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var outer: Tag; "
        "    do "
        "      var s := \"he\" + \"llo\"; "
        "      outer := Helper.Wrap(s); "
        "    end; "
        "    return (case outer of Named(v) => v.Length); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 5);
}

// ===========================================================================
// CONTRAST CONTROL — the issue's own stated contrast: the INLINE form of the
// identical value, with the same narrow-vs-wide scope shape, never reaches
// the #1430 relaxation at all (there is no intermediate string LOCAL for it
// to misclassify — the concatenation is either a literal, or a temporary
// consumed directly by the inner `new`). Clean before and after this fix;
// included to pin that the fix does not touch the shape the issue calls
// "round-trips correctly", not as a FAIL-WITHOUT row.
// ===========================================================================
BEZEL_TEST(record_choice_call_result_param_alias_uaf_4254,
           narrow_scope_inline_construction_control_stays_clean) {
    const char* prog =
        "module M; record Str(V: string); "
        "class C "
        "  public static method Main(): int "
        "    var outer: Str; "
        "    do "
        "      var s := \"he\" + \"llo\"; "
        "      outer := new Str(s); "
        "    end; "
        "    return outer.V.Length; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 5);
}

// ===========================================================================
// SCOPE-SHAPE CONTROL — the SAME helper-returned pattern as PIN 1, but in a
// single FLAT scope (no `do ... end;` narrowing `s`). The underlying
// accounting defect is identical (pre-fix, `s` is still wrongly classified
// non-escaping), but nothing ever reads the dangling alias before `s`'s own
// scope-end release fires — disposal runs only after the return expression
// is computed, so a flat caller can never OBSERVE this defect. Included to
// document why the issue's own two-line excerpt (no visible block scoping)
// needed a fuller repro to reproduce under ASan, and to pin that the fix
// does not change this row's value either. Clean before and after.
// ===========================================================================
BEZEL_TEST(record_choice_call_result_param_alias_uaf_4254,
           flat_scope_helper_returned_record_stays_clean_and_correct) {
    const char* prog =
        "module M; record Str(V: string); "
        "class Helper "
        "  public static method Wrap(s: string): Str "
        "    return new Str(s); "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var s := \"he\" + \"llo\"; "
        "    var outer := Helper.Wrap(s); "
        "    return outer.V.Length; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 5);
}

// ===========================================================================
// TYPE-KIND-BOUNDARY CONTROL — a CLASS-returning factory is the axis this
// fix deliberately does NOT touch: an explicit constructor's `this.f := x;`
// field store retains a borrowed string via `lowerStringFieldStore` (unlike
// a record's/choice's raw positional/payload store), so the #1430
// relaxation's soundness argument actually holds here. Same narrow-scope
// shape as PIN 1, with the CLASS's owned-string field manually disposed.
// Clean before and after — pins that the fix's Record/Choice gate does not
// over-reach into classes.
// ===========================================================================
BEZEL_TEST(record_choice_call_result_param_alias_uaf_4254,
           class_returning_factory_control_stays_clean_spec_28_3) {
    const char* prog =
        "module M; "
        "class Box "
        "  public V: string; "
        "  public constructor(v: string) "
        "    V := v; "
        "  end constructor; "
        "end Box; "
        "class Helper "
        "  public static method Wrap(s: string): owned Box "
        "    return new Box(s); "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var outer: Box; "
        "    do "
        "      var s := \"he\" + \"llo\"; "
        "      outer := Helper.Wrap(s); "
        "    end; "
        "    var r := outer.V.Length; "
        "    outer.Dispose(); "
        "    return r; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 5);
}
