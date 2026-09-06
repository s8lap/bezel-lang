// fix #3988 (§17.11/§9.3, v0.294): `ExprTC::defaultIllFormed`'s `Record` arm
// (and its sibling, `Resolver::argSatisfiesNew`'s `where T: new()` Record
// arm) walked ONLY the record's POSITIONAL fields — `td->positionalFields`
// off the AST, empty for every body-only record and empty for the body half
// of every combined-form record (§9.3: a record declaring both positional
// fields and a body). A non-nullable reference-typed BODY field or
// auto-property was therefore invisible to `defaultIllFormed`: `default(T)`
// over such a record was silently accepted, and codegen's recursive
// per-field default-construction null-derefs the manufactured null for that
// field at runtime — a compiler-accepted program that SIGSEGVs.
//
// MEASURED on this fix's parent (`git log` HEAD before this commit):
//
//   record Combo(X: int)              <- combined form, the issue's exact
//     public Extra: Widget;                repro (Widget: non-nullable class)
//   end Combo;
//   ...
//   var x := default(Combo);          <- compiled clean, exit 0
//   return x.Extra.Render();          <- exit 139, SIGSEGV
//
// And SEPARATELY (not named in the issue title, but the SAME root cause: the
// walk covers only POSITIONAL fields, so a body-ONLY record's storage was
// equally invisible — `td->positionalFields` is empty for a body-only record
// too):
//
//   record BodyRec2
//     public readonly property Extra: Widget;   <- body-only, an AUTO-PROPERTY
//     public constructor(w: Widget) Extra := w; end constructor;
//   end BodyRec2;
//   ...
//   var x := default(BodyRec2);       <- compiled clean, exit 0
//   return x.Extra.Render();          <- exit 139, SIGSEGV
//
// §17.11 (v0.294) reads: "Records all of whose declared fields are
// defaultable ... For positional records, the positional fields are
// considered. For body-only records, the backing storage of all
// auto-properties is considered." — i.e. §17.11's own body-only-record
// bullet was NEVER implemented (the AST walk only ever read positional
// fields), a strictly wider gap than the issue's combined-form framing.
//
// READING (recorded per CLAUDE.md rule 4, "decline-with-reasons"): §17.11
// does not literally spell out the COMBINED form (§9.3), but its opening
// clause ("Records all of whose DECLARED FIELDS are defaultable") is
// general, and §9.3 itself states the identical union rule for the
// analogous admissible-component-type check: "the positional fields counted
// as storage components alongside the body's auto-properties". This fix
// reads §17.11's combined-form case the same way — positional fields UNION
// body storage (auto-properties AND explicit field declarations, both of
// which are "declared fields" of the record; §9.2 admits an explicit field
// declaration as storage, disqualifying only *auto-hashable* status, which
// is unrelated to `default` eligibility) — rather than stopping to file an
// issue over it, because §9.3's own precedent settles the union reading.
//
// THE FIX routes both `ExprTC::defaultIllFormed`'s Record arm
// (ExprTypecheck_Exprs.cpp) and `Resolver::argSatisfiesNew`'s Record arm
// (TypeResolution_Checks.cpp, the `where T: new()` sibling, which shared the
// identical positional-only gap and explicitly deferred body-field coverage
// to "the default-operator slice" — a deferral that was never actually
// honored) through `recordStorageMembers` (sema/Symbol.hpp) — the ONE
// symbol-side full-storage enumeration (positional fields, body fields, and
// auto-properties together), which already answers identically for a local
// and an IMPORTED record (see xmod_default_record_combined_form_body_field_
// 3988_test.cpp for the cross-module twin).
//
// FAIL-WITHOUT / PASS-WITH (proven mechanically): with `src/sema/
// ExprTypecheck_Exprs.cpp` and `src/sema/TypeResolution_Checks.cpp` reverted
// to this fix's parent, every REJECT-expecting test below instead compiles
// clean (`r.ran` observed true, wrong exit code) or accepts the `where T:
// new()` instantiation; restored, every REJECT test asserts a real §17.11 /
// §14.2 location and every ACCEPT test's positive control still compiles
// and returns the value asserted.
//
// Filters run (no --area): `default_t` / `17_11` / `combined_form` /
// `record_field` / `new_constraint` / `generic_record` — see the commit
// body for exact pass counts and which binaries each hit. A wide pre-fix
// sweep of these same filters over the PRE-EXISTING test corpus (before
// this file existed) showed ZERO existing tests relying on the old
// (incorrect) admission — false-reject cost measured as 0.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

const char* kDefaultRejectMsg =
    "`default(T)` is a compile error for a record any of whose fields is a "
    "non-nullable reference type";

// Asserts a clean, real-location §17.11 rejection of `default(T)` over
// `program`, never a runtime crash (the exact class of defect: pre-fix this
// compiled clean and SIGSEGV'd at runtime).
void expectDefaultRecordRejected(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kDefaultRejectMsg));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""17.11"));
}

const char* kWidget =
    "class Widget "
    "  public Count: int; "
    "  public constructor(c: int) Count := c; end constructor; "
    "  public method Render(): int return Count; end Render; "
    "end Widget; ";

}  // namespace

// ── (1) the issue's exact repro: combined-form record, plain explicit body
//        field of non-nullable class type ─────────────────────────────────
BEZEL_TEST(default_record_combined_form_body_field_3988,
           combined_form_explicit_body_field_rejected_spec_17_11) {
    expectDefaultRecordRejected(
        std::string("module M; ") + kWidget +
        "record Combo(X: int) "
        "  public Extra: Widget; "
        "end Combo; "
        "class Program "
        "  public static method Main(): int "
        "    var x := default(Combo); "
        "    return x.Extra.Render(); "
        "  end Main; "
        "end Program;");
}

// ── (2) body-ONLY record, auto-property of non-nullable class type — a
//        STRICTLY WIDER finding than the issue's title: §17.11's own
//        body-only-record bullet was never implemented either, since the
//        positional-only walk is empty for a body-only record too ────────
BEZEL_TEST(default_record_combined_form_body_field_3988,
           body_only_auto_property_rejected_spec_17_11) {
    expectDefaultRecordRejected(
        std::string("module M; ") + kWidget +
        "record BodyRec "
        "  public readonly property Extra: Widget; "
        "  public constructor(w: Widget) Extra := w; end constructor; "
        "end BodyRec; "
        "class Program "
        "  public static method Main(): int "
        "    var x := default(BodyRec); "
        "    return x.Extra.Render(); "
        "  end Main; "
        "end Program;");
}

// ── (3, anti-regression) purely positional twin — must keep rejecting
//        exactly as before (this arm was never broken) ────────────────────
BEZEL_TEST(default_record_combined_form_body_field_3988,
           positional_only_twin_still_rejects_spec_17_11) {
    expectDefaultRecordRejected(
        std::string("module M; ") + kWidget +
        "record ComboPos(X: int, Extra: Widget) "
        "end ComboPos; "
        "class Program "
        "  public static method Main(): int "
        "    var x := default(ComboPos); "
        "    return x.X; "
        "  end Main; "
        "end Program;");
}

// ── (4, positive control) combined-form record, NULLABLE explicit body
//        field — a valid null source, must keep compiling AND running, with
//        the value actually observed at runtime ───────────────────────────
BEZEL_TEST(default_record_combined_form_body_field_3988,
           combined_form_nullable_body_field_positive_control) {
    auto r = runAot(
        std::string("module M; ") + kWidget +
        "record ComboN(X: int) "
        "  public Extra: Widget?; "
        "end ComboN; "
        "class Program "
        "  public static method Main(): int "
        "    var x := default(ComboN); "
        "    if x.Extra <> null then return 1; end if; "
        "    return 7; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ── (5, positive control) combined-form record, VALUE-typed explicit body
//        field — must keep compiling and running with the zero value ──────
BEZEL_TEST(default_record_combined_form_body_field_3988,
           combined_form_value_typed_body_field_positive_control) {
    auto r = runAot(
        "module M; "
        "record ComboV(X: int) "
        "  public Extra: int; "
        "end ComboV; "
        "class Program "
        "  public static method Main(): int "
        "    var x := default(ComboV); "
        "    return x.X + x.Extra; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── (6, positive control) combined-form record, NULLABLE auto-property (as
//        opposed to test (4)'s plain field) — both storage shapes route
//        through the same `recordStorageMembers` enumeration ──────────────
BEZEL_TEST(default_record_combined_form_body_field_3988,
           combined_form_nullable_auto_property_positive_control) {
    auto r = runAot(
        std::string("module M; ") + kWidget +
        "record ComboAP(X: int) "
        "  public readonly property Extra: Widget?; "
        "end ComboAP; "
        "class Program "
        "  public static method Main(): int "
        "    var x := default(ComboAP); "
        "    if x.Extra <> null then return 1; end if; "
        "    return 7; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ── (7) a combined-form record's body field that is ITSELF a record with a
//        non-defaultable positional field — the recursive `defaultIllFormed`
//        call on the body field's type must still recurse correctly ───────
BEZEL_TEST(default_record_combined_form_body_field_3988,
           nested_record_body_field_rejected_spec_17_11) {
    expectDefaultRecordRejected(
        std::string("module M; ") + kWidget +
        "record Inner(W: Widget) "
        "end Inner; "
        "record Outer(X: int) "
        "  public In: Inner; "
        "end Outer; "
        "class Program "
        "  public static method Main(): int "
        "    var x := default(Outer); "
        "    return x.X; "
        "  end Main; "
        "end Program;");
}

// ── (8) an inline array of combined-form records with a non-nullable body
//        field — the InlineArray arm's recursion into the element type must
//        still see the body-field defect ──────────────────────────────────
BEZEL_TEST(default_record_combined_form_body_field_3988,
           inline_array_of_combined_form_records_rejected_spec_17_11) {
    auto r = runAot(
        std::string("module M; ") + kWidget +
        "record Combo(X: int) "
        "  public Extra: Widget; "
        "end Combo; "
        "class Program "
        "  public static method Main(): int "
        "    var arr := default([3]Combo); "
        "    return arr[0].X; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r,
        "`default(T)` is a compile error for an inline array whose element "
        "type is a non-nullable reference type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""17.11"));
}

// ── (9) the sibling `where T: new()` check (Resolver::argSatisfiesNew, §14.2)
//        must reject a combined-form record with a non-nullable body field
//        as a `new()`-constraint type argument, exactly like `default(T)` ──
BEZEL_TEST(default_record_combined_form_body_field_3988,
           where_new_constraint_rejects_combined_form_body_field_spec_14_2) {
    auto r = runAot(
        std::string("module M; ") + kWidget +
        "record Combo(X: int) "
        "  public Extra: Widget; "
        "end Combo; "
        "class Holder<T> where T: new() "
        "  public static method MakeAndUse(): int "
        "    _ := new T(); "
        "    return 0; "
        "  end MakeAndUse; "
        "end Holder; "
        "class Program "
        "  public static method Main(): int "
        "    return Holder<Combo>.MakeAndUse(); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r,
        "does not satisfy the constraint `where T: new()`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""14.2"));
}

// ── (10, positive control) `where T: new()` must still ACCEPT a
//        combined-form record whose body field is nullable ────────────────
BEZEL_TEST(default_record_combined_form_body_field_3988,
           where_new_constraint_accepts_nullable_body_field_spec_14_2) {
    auto r = runAot(
        std::string("module M; ") + kWidget +
        "record ComboN(X: int) "
        "  public Extra: Widget?; "
        "end ComboN; "
        "class Holder<T> where T: new() "
        "  public static method MakeAndUse(): int "
        "    _ := new T(); "
        "    return 0; "
        "  end MakeAndUse; "
        "end Holder; "
        "class Program "
        "  public static method Main(): int "
        "    return Holder<ComboN>.MakeAndUse(); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── (11) DELEGATING generic record through a COMBINED-FORM body field
//        (§3.6 rule 2 / §17.11 / #3995): `record Inner<T>(V: T)` and
//        `record Outer<T>(X: int) { public In: Inner<T>; }` — Outer's body
//        field `In`'s declared type is the CONCRETE-LOOKING `Inner<T>`
//        (not itself a bare parameter), but `T` inside it is a bare mention
//        of Outer's OWN parameter, resolving only if the recursive
//        `defaultIllFormed` call into Inner's frame is seeded with Outer's
//        environment as `incoming` (fix #3995's composition). This is the
//        exact intersection this fix's own storage-shape widening
//        (`recordStorageMembers` over `recordPositionalFieldSymbols`) must
//        preserve: `recordStorageMembers` is called with `env=nullptr` so
//        the RAW (unsubstituted) field type is what this arm's own
//        `resolveRecordFieldSubst` + `Entry::env`-threaded recursion sees —
//        losing that env thread here would silently reintroduce #3995 for
//        a DELEGATING field reached through body/combined-form storage,
//        the one shape neither #3986's nor #3988's own suite covered alone.
//
// MEASURED on this fix's immediate parent (the #3986/#3982/#3995-merged arm
// BEFORE this fix's `recordStorageMembers` swap, i.e. the positional-only
// walk with correct #3995 composition): `default(Outer<Widget>)` for this
// COMBINED-FORM `Outer` compiled clean (exit 0) and SIGSEGV'd (exit 139) —
// the positional-only walk never even reaches the body field `In` to
// substitute it. Confirmed identically for the plain-field BODY-ONLY twin
// below.
BEZEL_TEST(default_record_combined_form_body_field_3988,
           delegating_generic_record_combined_form_body_field_rejected_spec_17_11_3995) {
    expectDefaultRecordRejected(
        std::string("module M; ") + kWidget +
        "record InnerDel<T>(V: T) "
        "end InnerDel; "
        "record OuterDelCF<T>(X: int) "
        "  public In: InnerDel<T>; "
        "end OuterDelCF; "
        "class Program "
        "  public static method Main(): int "
        "    var x := default(OuterDelCF<Widget>); "
        "    return x.In.V.Render(); "
        "  end Main; "
        "end Program;");
}

// ── (12) same delegating shape through BODY-ONLY storage (a plain explicit
//        field, not an auto-property, to keep the observation unconfounded
//        by the unrelated #2884/#3994 imported-generic-accessor limitations
//        the cross-module twin below hits for a property) ─────────────────
BEZEL_TEST(default_record_combined_form_body_field_3988,
           delegating_generic_record_body_only_field_rejected_spec_17_11_3995) {
    expectDefaultRecordRejected(
        std::string("module M; ") + kWidget +
        "record InnerDelBO<T>(V: T) "
        "end InnerDelBO; "
        "record OuterDelBO<T> "
        "  public In: InnerDelBO<T>; "
        "end OuterDelBO; "
        "class Program "
        "  public static method Main(): int "
        "    var x := default(OuterDelBO<Widget>); "
        "    return x.In.V.Render(); "
        "  end Main; "
        "end Program;");
}

// ── (13, positive control) the SAME delegating combined-form shape,
//        substituted at a VALUE type — must stay accepted with the correct
//        value, proving the composition doesn't over-reject ──────────────
BEZEL_TEST(default_record_combined_form_body_field_3988,
           delegating_generic_record_combined_form_value_leaf_positive_control) {
    auto r = runAot(
        "module M; "
        "record InnerDelV<T>(V: T) "
        "end InnerDelV; "
        "record OuterDelCFV<T>(X: int) "
        "  public In: InnerDelV<T>; "
        "end OuterDelCFV; "
        "class Program "
        "  public static method Main(): int "
        "    var x := default(OuterDelCFV<int>); "
        "    return x.X + x.In.V; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── (14, positive control) the SAME delegating body-only shape, substituted
//        at a NULLABLE class leaf — must stay accepted (null) ─────────────
BEZEL_TEST(default_record_combined_form_body_field_3988,
           delegating_generic_record_body_only_nullable_leaf_positive_control) {
    auto r = runAot(
        std::string("module M; ") + kWidget +
        "record InnerDelBON<T>(V: T) "
        "end InnerDelBON; "
        "record OuterDelBON<T> "
        "  public In: InnerDelBON<T>; "
        "end OuterDelBON; "
        "class Program "
        "  public static method Main(): int "
        "    var x := default(OuterDelBON<Widget?>); "
        "    if x.In.V <> null then return 1; end if; "
        "    return 9; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}
