// fix #4166 (§14.2 "Constraints" / §17.11 "The default operator", v0.298):
// `Resolver::argSatisfiesNew`'s Record arm (src/sema/TypeResolution_Checks.cpp)
// — fixed for a BARE generic field by #4002 — never checked a field that is
// ITSELF a record after substitution. Its per-member test ran
// `categorize(subFt) == TypeCategory::Reference`; `categorize()` answers
// "Record ⇒ Value" for ANY record, unconditionally — true of the record AS A
// TYPE but silent on whether the record's OWN fields are themselves
// defaultable. So a DELEGATING record field (`In: Inner<T>` in
// `record Outer<T>(In: Inner<T>)`) was never recursed into: its inner leaf's
// own non-defaultability was invisible, `where T: new()` was satisfied
// unverified, and the compiler-generated `new T()` SIGSEGV'd at runtime —
// the same failure class #3995 (ExprTC::defaultIllFormed) and #4002 both
// fixed for their own shallower/one-level cases.
//
// Flagged, but never fixed or tracked, by #4002's own test file's "OUT OF
// SCOPE" note (tests/sema/arg_satisfies_new_generic_record_subst_4002_test.cpp
// lines 74-85), which named the fix shape: port #3995's `Entry::env`
// composition into `argSatisfiesNew`. Tracked as #4166; this fixes it.
//
// THE GOVERNING SPEC TEXT (§14.2 "Constraints", v0.298, quoted in full for
// the `new()` value-type arm — the exact bullet this fix implements):
//
//   "A value-type substitution satisfies the constraint exactly when it is
//   defaultable (§17.11). Inside the generic body, `new T()` for a value
//   substitution is equivalent to `default(T)`: for records this is the
//   zero-valued record (admitting `record Point(X: int, Y: int)` while
//   rejecting records with non-nullable reference fields); for enums it is
//   the zero value — an enum with no declared zero-valued constant is not
//   defaultable (§17.11) and does not satisfy the constraint. ... Interface
//   substitutions never satisfy `new()` — no constructor exists. Choices
//   never satisfy the value-type arm — a consequence of the property: no
//   choice is defaultable (§17.11, §35.10)."
//
// And §17.11's "defaultable" glossary entry (docs/Bezel_Language_Spec.md
// line ~534, v0.298), the RECURSIVE definition this fix ports into
// `argSatisfiesNew`:
//
//   "types for which `default(T)` is defined: every nullable type `T?`;
//   and every non-nullable value type whose components are recursively
//   defaultable — the primitives, the unit type, an enum declaring a
//   zero-valued constant, a record all of whose declared fields are
//   defaultable and which is not constructor-only (§9.9), an inline array
//   whose element type is defaultable ... Not defaultable: non-nullable
//   reference types; every choice type (§35.10); an enum with no
//   zero-valued constant; a constructor-only record (§9.9); and any record
//   or inline array with a non-defaultable component."
//
// This fix implements ONLY the "record all of whose declared fields are
// defaultable" recursion for a delegating RECORD field (a field that is
// itself a record) — it does not change the pre-existing leaf test
// (`categorize() == Reference`) for a Choice- or Enum-without-zero-typed
// field, which is a separate, non-generics-specific gap in the SAME leaf
// test filed separately (see the commit body / issue tracker).
//
// MEASURED on this fix's parent (r18 integration head `1e6d80f`, before this
// fix — the issue's own repro, reproduced verbatim):
//
//     module MArgNewDelegate;
//     public class WidgetAND
//       public Count: int;
//       public constructor(c: int) Count := c; end constructor;
//     end WidgetAND;
//     public record InnerAND<T>(V: T);
//     public record OuterAND<T>(I: InnerAND<T>);
//     class HolderAND<T> where T: new()
//       public static method Make(): owned T => new T();
//     end HolderAND;
//     class ProgAND
//       public static method Main(): int
//         var b := HolderAND<OuterAND<WidgetAND>>.Make();
//         return b.I.V.Count;
//       end Main;
//     end ProgAND;
//
//   compiled at exit 0 with NO diagnostic, then SIGSEGV'd (exit 139) at
//   runtime — `b.I.V` is a null pointer manufactured by codegen's `new T()`
//   record/field lowering, which recursively default-constructs each field
//   on the assumption sema already rejected any field for which that has no
//   valid value.
//
// THE FIX (first version, commit 71dc4ce) added a Resolver-local method,
// `recordFieldTypeHasNoDefault`, which recursed into a Record-kind field
// instead of stopping at `categorize()`, threading
// `resolveRecordFieldSubst`'s `Entry::env` into the inner frame — the SAME
// composition `ExprTC::defaultIllFormed` threads for the identically-shaped
// `default(T)` well-formedness question (fix #3995). `Resolver::run` (type
// resolution) executes before `ExprTC` exists (Driver.cpp: `resolveTypes`
// then `typecheckExpressions`), so it could not simply call
// `defaultIllFormed` itself.
//
// REVIEW FOLLOW-UP #1 (§9/§35.1 cycle guard, v0.298): that first version
// recursed with no cycle guard — `ExprTC::defaultIllFormed`, the function
// it was ported from, had none either, and shared the same hazard via
// `default(...)`. A self-referential record
// (`record Self<T>(N: Self<int>, V: T)`, already independently rejected
// by `recordValueContainsSelf` at declaration time) recurses unboundedly
// through field `N` and SIGSEGVs, because `Resolver::run` still evaluates
// `where T: new()` after that earlier rejection. Before this port, only an
// explicit `default(SelfReferentialRecord<...>)` could reach the
// unguarded recursion; after, ANY `where T: new()` instantiation site can
// (generic factories, DI-style code) — a much larger trigger surface, so
// the guard was required here even though the sibling it was ported from
// still lacked one at that point. See row (8) below.
//
// REVIEW FOLLOW-UP #2 (§14.2/§17.11/§35.10, v0.298): the Resolver-local
// pair was then REPLACED with a SHARED free function,
// `sema::typeHasNoDefault` (TypeResolution.hpp / TypeResolution_Checks.cpp),
// which BOTH `Resolver::argSatisfiesNew`'s Record arm and
// `ExprTC::defaultIllFormed` now call — a faithful, EXTENDED port of
// `defaultIllFormed`'s full 8-way per-kind switch (not just its Record-kind
// recursion, which is all the first version ported), gaining the cycle
// guard as a shared PATH-semantics `std::unordered_set<const TypeRef*>`
// (insert-on-entry/erase-on-exit) mirroring
// `common::isBorrowBearingRecordCommon` (OwnershipFacts.cpp). This closed
// two more divergences the review's line-by-line comparison found between
// the two independently-drifting copies — Enum/Choice/InlineArray handling
// and the `isValueBuiltin`-aware Class/Interface test (issues #4219,
// #4223) — and means `ExprTC::defaultIllFormed`'s OWN pre-existing
// self-referential-record SIGSEGV (via `default(...)`, not just
// `where T: new()`) is fixed too, since it now shares this same guard.
//
// SHAPE SWEEP (every row measured before/after the fix on this fix's
// parent; see the commit body for the FAIL-WITHOUT / PASS-WITH counts):
//   - delegating generic record, inner leaf bound to a non-nullable class
//       -> REJECT (§14.2/§17.11) — the issue's exact repro;
//          `where_T_new_rejects_delegating_generic_record_bound_to_non_nullable_class`
//   - delegating generic record, inner leaf bound to a value type (`int`)
//       -> ACCEPT, runtime value 0
//          (`where_T_new_accepts_delegating_generic_record_bound_to_int`)
//   - delegating generic record, inner leaf bound to a nullable class
//       -> ACCEPT, runtime value null
//          (`where_T_new_accepts_delegating_generic_record_bound_to_nullable_class`)
//   - THREE-LEVEL delegation chain (Outer -> Middle -> Inner), inner leaf
//       non-nullable class -> REJECT, proving the recursion composes past
//       one level, not just two
//       (`where_T_new_rejects_three_level_delegating_chain`)
//   - NON-GENERIC delegating record (neither Outer nor Inner is generic at
//       all — the issue's own diagnosis: "never checked, generic or not")
//       whose innermost leaf is a non-nullable class -> REJECT, proving the
//       gap was never specific to genericity
//       (`where_T_new_rejects_nongeneric_delegating_record`)
//   - REGRESSION CONTROL: #4002's own bare-parameter (non-delegating)
//       generic record shape keeps working under the walk now used for
//       every member
//       (`where_T_new_rejects_bare_generic_record_4002_control`)
//   - REGRESSION CONTROL: #3988's combined-form body-field shape (no
//       generics, no delegation) keeps working
//       (`where_T_new_rejects_combined_form_body_field_3988_control`)
//   - CYCLE GUARD: a self-referential record reached through
//       `where T: new()` -> REJECT with the CLEAN two-diagnostic outcome
//       (§9 self-containment + §14.2 constraint), not a crash
//       (`where_T_new_self_referential_record_rejects_cleanly_no_crash`)
//   - SHARED-FUNCTION UNIFICATION: a record field of Choice type -> REJECT
//       (`where_T_new_rejects_record_with_choice_field`, closes #4219);
//       a record field of a zero-less Enum -> REJECT
//       (`where_T_new_rejects_record_with_zeroless_enum_field`, closes
//       #4219); a record field that is an inline array of a
//       non-defaultable generic record -> REJECT
//       (`where_T_new_rejects_record_with_inline_array_of_generic_record_field`,
//       closes #4223); a record field of non-nullable `Object` -> REJECT
//       (`where_T_new_rejects_record_with_object_field`, regression
//       control for the divergence this fix's own broad sweep caught);
//       a record field of builtin value type `DateTime` -> ACCEPT
//       (`where_T_new_accepts_record_with_datetime_field`, positive
//       control proving the `isValueBuiltin`-aware test doesn't
//       over-reject)
//   - BUILTIN-COLLECTION REGRESSION (review follow-up #3): a record field
//       of `Array<int>` / `Dictionary<string,int>` / `Set<int>` ->
//       REJECT under `where T: new()`
//       (`where_T_new_rejects_record_with_array_field`,
//       `..._dictionary_field`, `..._set_field` — a null-declaration
//       Named type is not only an unresolved generic parameter)
//   - `default(...)`-SIDE cycle guard + Object rows (review-confirmed side
//       findings): the row (8) self-reference shape reached via
//       `default(...)` instead of `where T: new()` -> REJECT, clean, not a
//       crash (`default_operator_self_referential_record_rejects_
//       cleanly_no_crash`); a record field of non-nullable `Object`
//       reached via `default(...)` -> REJECT
//       (`default_operator_rejects_record_with_object_field`)
//   - POSITIVE CONTROL for the builtin-collection fix's own near-miss: a
//       record field of `Guid` (a VALUE builtin, ALSO null-declaration,
//       but with a real default) -> ACCEPT under `where T: new()`
//       (`where_T_new_accepts_record_with_guid_field` — this is the row
//       that would have failed had the fix mirrored `categorize()`'s
//       builtin fallback wholesale instead of scoping it by name to the
//       collection/`Task` family)
//   - FFI-BOUNDARY CLASS REGRESSION (review-prompted, MEASURED including a
//       live SIGSEGV on the parent): `CString`/`WString`/`RawBytes` ->
//       REJECT under `where T: new()`
//       (`where_T_new_rejects_record_with_cstring_field`, `..._wstring_field`,
//       `..._rawbytes_field`); the exact `default(...)` shape that
//       SIGSEGV'd on parent `1e6d80f` -> clean REJECT instead
//       (`default_operator_rejects_record_with_cstring_field_no_crash`)
//   - POSITIVE CONTROL, second half: `IntPtr` (grouped with CString/
//       WString/RawBytes by `isFfiBoundaryNamedType` for the FFI-
//       compatibility question, but a VALUE type with a real default) ->
//       ACCEPT (`where_T_new_accepts_record_with_intptr_field` — the row
//       that would have failed had the fix reused that predicate wholesale)

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// Widget4166: non-nullable class with a primary-constructor parameter —
// every REJECT row below binds a delegating record's innermost leaf to this
// class, whose non-nullable-reference field has no well-defined default
// (§17.11).
const char* kWidget4166 =
    "public class Widget4166 "
    "  public Count: int; "
    "  public constructor(c: int) Count := c; end constructor; "
    "end Widget4166; ";

// Asserts a clean, real-location §14.2 rejection of `Holder<...>` over
// `program`, never a runtime crash (the exact class of defect: pre-fix this
// compiled clean and SIGSEGV'd at runtime).
void expectWhereNewRejected(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r,
        "does not satisfy the constraint `where T: new()`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""14.2"));
}

}  // namespace

// ── (1) the issue's exact repro: a delegating generic record
//        (`record Outer<T>(I: Inner<T>)`), inner leaf bound to a
//        non-nullable class ──────────────────────────────────────────────
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_rejects_delegating_generic_record_bound_to_non_nullable_class_spec_14_2) {
    expectWhereNewRejected(
        std::string("module M4166a; ") + kWidget4166 +
        "public record Inner4166a<T>(V: T); "
        "public record Outer4166a<T>(I: Inner4166a<T>); "
        "class Holder4166a<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166a; "
        "class Program4166a "
        "  public static method Main(): int "
        "    var b := Holder4166a<Outer4166a<Widget4166>>.Make(); "
        "    return b.I.V.Count; "
        "  end Main; "
        "end Program4166a;");
}

// ── (2) positive control: delegating record, inner leaf bound to a value
//        type (`int`) — `new T()` for both frames is `0`, the constraint
//        IS satisfied, no diagnostic ─────────────────────────────────────
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_accepts_delegating_generic_record_bound_to_int_spec_14_2) {
    auto r = runAot(
        "module M4166b; "
        "public record Inner4166b<T>(V: T); "
        "public record Outer4166b<T>(I: Inner4166b<T>); "
        "class Holder4166b<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166b; "
        "class Program4166b "
        "  public static method Main(): int "
        "    var b := Holder4166b<Outer4166b<int>>.Make(); "
        "    return b.I.V; "
        "  end Main; "
        "end Program4166b;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── (3) positive control: delegating record, inner leaf bound to a
//        NULLABLE class type — the constraint IS satisfied (null) ────────
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_accepts_delegating_generic_record_bound_to_nullable_class_spec_14_2) {
    auto r = runAot(
        std::string("module M4166c; ") + kWidget4166 +
        "public record Inner4166c<T>(V: T); "
        "public record Outer4166c<T>(I: Inner4166c<T>); "
        "class Holder4166c<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166c; "
        "class Program4166c "
        "  public static method Main(): int "
        "    var b := Holder4166c<Outer4166c<Widget4166?>>.Make(); "
        "    _ := b; "
        "    return 0; "
        "  end Main; "
        "end Program4166c;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── (4) THREE-level delegation chain (Outer -> Middle -> Inner): proves the
//        composed env threads through more than one nested frame, not just
//        the two the issue's own repro exercises ──────────────────────────
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_rejects_three_level_delegating_chain_spec_14_2) {
    expectWhereNewRejected(
        std::string("module M4166d; ") + kWidget4166 +
        "public record Inner4166d<T>(V: T); "
        "public record Middle4166d<T>(M: Inner4166d<T>); "
        "public record Outer4166d<T>(O: Middle4166d<T>); "
        "class Holder4166d<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166d; "
        "class Program4166d "
        "  public static method Main(): int "
        "    var b := Holder4166d<Outer4166d<Widget4166>>.Make(); "
        "    return b.O.M.V.Count; "
        "  end Main; "
        "end Program4166d;");
}

// ── (5) NON-GENERIC delegating record: neither Outer nor Inner is generic
//        at all — the issue's own diagnosis was that the gap is "never
//        checked, generic or not". Confirms the fix is not generics-gated —
//        even before this fix, `categorize(Inner4166e) == Value` for ANY
//        record, so this shape was ALSO silently accepted pre-fix. ────────
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_rejects_nongeneric_delegating_record_spec_14_2) {
    expectWhereNewRejected(
        std::string("module M4166e; ") + kWidget4166 +
        "public record Inner4166e(V: Widget4166); "
        "public record Outer4166e(I: Inner4166e); "
        "class Holder4166e<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166e; "
        "class Program4166e "
        "  public static method Main(): int "
        "    var b := Holder4166e<Outer4166e>.Make(); "
        "    return b.I.V.Count; "
        "  end Main; "
        "end Program4166e;");
}

// ── (6) REGRESSION CONTROL: #4002's own bare-parameter (non-delegating)
//        generic-record coverage must keep working — same shape as
//        `arg_satisfies_new_generic_record_subst_4002_test.cpp` row (1) ──
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_rejects_bare_generic_record_4002_control_spec_14_2) {
    expectWhereNewRejected(
        std::string("module M4166f; ") + kWidget4166 +
        "public record Box4166f<T>(V: T); "
        "class Holder4166f<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166f; "
        "class Program4166f "
        "  public static method Main(): int "
        "    var b := Holder4166f<Box4166f<Widget4166>>.Make(); "
        "    return b.V.Count; "
        "  end Main; "
        "end Program4166f;");
}

// ── (7) REGRESSION CONTROL: #3988's combined-form body-field coverage
//        (no generics, no delegation) must keep working — same shape as
//        `arg_satisfies_new_generic_record_subst_4002_test.cpp` row (4) ──
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_rejects_combined_form_body_field_3988_control_spec_14_2) {
    expectWhereNewRejected(
        std::string("module M4166g; ") + kWidget4166 +
        "record Combo4166g(X: int) "
        "  public Extra: Widget4166; "
        "end Combo4166g; "
        "class Holder4166g<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166g; "
        "class Program4166g "
        "  public static method Main(): int "
        "    var b := Holder4166g<Combo4166g>.Make(); "
        "    return b.Extra.Count; "
        "  end Main; "
        "end Program4166g;");
}

// ── (8) CYCLE GUARD (review follow-up, §9/§35.1, v0.298): a malformed
//        self-referential record (`record Self<T>(N: Self<int>, V: T)`) —
//        already rejected independently by `recordValueContainsSelf` at
//        declaration time (§9) — reached through `where T: new()`.
//        WITHOUT a cycle guard in the field-defaultability walk's
//        Record-kind recursion (`sema::typeHasNoDefaultWalk`,
//        TypeResolution_Checks.cpp), field `N`'s declaration-site type
//        `Self<int>` is the SAME `TypeRef*` on every recursive step (a
//        NAMED, non-bare field is never substituted to a new pointer by
//        `resolveRecordFieldSubst`), so the walk never terminates —
//        SIGSEGV. `Resolver::run` still evaluates `where T: new()` AFTER
//        the §9 rejection, so this shape is reachable regardless of that
//        earlier diagnostic.
//
//        Asserts the CLEAN two-diagnostic rejection, not merely "no
//        crash": field `N`'s cycle must DEFER (return "has a default",
//        not "non-defaultable") so the walk still reaches field `V`,
//        whose binding to a non-nullable class correctly triggers the
//        §14.2 diagnostic too — exactly the two diagnostics the pre-#4166
//        code (which never recursed into `N` at all, so never hit this
//        hazard, but also never caught a genuinely delegating record)
//        produced for this shape. ──────────────────────────────────────
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_self_referential_record_rejects_cleanly_no_crash_spec_9_14_2) {
    auto r = runAot(
        std::string("module M4166h; ") + kWidget4166 +
        "public record SelfRP2h<T>(N: SelfRP2h<int>, V: T); "
        "class Holder4166h<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166h; "
        "class Program4166h "
        "  public static method Main(): int "
        "    var b := Holder4166h<SelfRP2h<Widget4166>>.Make(); "
        "    return b.V.Count; "
        "  end Main; "
        "end Program4166h;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // §9 (checkRecordRestrictions / recordValueContainsSelf): the record's
    // own value-self-containment, rejected independently of this fix.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "cannot recursively contain itself by value"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9)"));
    // §14.2: the `where T: new()` constraint, correctly still evaluated —
    // and still rejected, via field `V` — after field `N`'s cycle is
    // safely deferred rather than crashing.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "does not satisfy the constraint `where T: new()`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""14.2"));
}

// ── (9)-(13) SHARED-FUNCTION UNIFICATION (review follow-up #2, §14.2/
//        §17.11/§35.10, v0.298): `argSatisfiesNew`'s Record-arm field walk
//        now goes through `sema::typeHasNoDefault`'s FULL per-kind switch
//        (ported from `ExprTC::defaultIllFormed`) instead of the narrower
//        `categorize() == Reference` leaf test the first two fixes used.
//        These rows exercise the field kinds that switch adds coverage
//        for — a record field that is a Choice, an Enum with no
//        zero-valued constant, an inline array of a non-defaultable
//        generic record, and a non-nullable `Object` (closes issues
//        #4219, #4223, and the `Object` divergence this fix's own broad
//        sweep caught against `xmod_imported_positional_new_args_3141`)
//        — plus a positive control proving the `isValueBuiltin`-aware
//        Class/Interface test does NOT over-reject a builtin value type
//        with a real default (`DateTime`). ──────────────────────────────

// (9) a record field of CHOICE type: §35.10 says no choice is
//     defaultable, unconditionally — `categorize()` alone (Choice ⇒
//     Value) never caught this.
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_rejects_record_with_choice_field_spec_14_2_35_10) {
    expectWhereNewRejected(
        std::string("module M4166i; ") + kWidget4166 +
        "choice Opt4166i "
        "  Some(V: Widget4166), None; "
        "end Opt4166i; "
        "public record BoxChoice4166i(O: Opt4166i); "
        "class Holder4166i<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166i; "
        "class Program4166i "
        "  public static method Main(): int "
        "    var b := Holder4166i<BoxChoice4166i>.Make(); "
        "    return (case b.O of Some(w) => w.Count, _ => -1); "
        "  end Main; "
        "end Program4166i;");
}

// (10) a record field of an ENUM with NO zero-valued constant: §17.11
//      says such an enum is not defaultable — `categorize()` alone
//      (Enum ⇒ Value) never caught this either.
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_rejects_record_with_zeroless_enum_field_spec_14_2_17_11) {
    expectWhereNewRejected(
        "module M4166j; "
        "enum Priority4166j "
        "  Low := 1, High := 2; "
        "end Priority4166j; "
        "public record BoxEnum4166j(P: Priority4166j); "
        "class Holder4166j<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166j; "
        "class Program4166j "
        "  public static method Main(): int "
        "    var b := Holder4166j<BoxEnum4166j>.Make(); "
        "    return 0; "
        "  end Main; "
        "end Program4166j;");
}

// (11) a record field that is an INLINE ARRAY of a non-defaultable
//      generic record — the InlineArray recursion arm (dropped entirely
//      by the `categorize()`-only leaf test, since `categorize` answers
//      `InlineArray ⇒ Value` with no recursion into the element type).
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_rejects_record_with_inline_array_of_generic_record_field_spec_14_2_17_11) {
    expectWhereNewRejected(
        std::string("module M4166k; ") + kWidget4166 +
        "public record Box4166k<T>(V: T); "
        "public record ArrHolder4166k(Arr: [2]Box4166k<Widget4166>); "
        "class Holder4166k<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166k; "
        "class Program4166k "
        "  public static method Main(): int "
        "    var b := Holder4166k<ArrHolder4166k>.Make(); "
        "    return b.Arr[0].V.Count; "
        "  end Main; "
        "end Program4166k;");
}

// (12) a record field of non-nullable `Object`: a REGRESSION CONTROL for
//      the divergence this fix's own broad sweep caught — unifying
//      toward `defaultIllFormed`'s ORIGINAL text (which deliberately
//      excluded `Object`, a documented #951-era scope decision) would
//      have silently reintroduced a false accept here, since
//      `argSatisfiesNew`'s pre-existing `categorize()`-based test already
//      correctly rejected it (categorize: Object ⇒ Reference). Mirrors
//      `xmod_imported_positional_new_args_3141_test.cpp`'s
//      `RefRec3141(R: Object, N: int)` shape (module-local here instead
//      of cross-module).
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_rejects_record_with_object_field_spec_14_2_17_11) {
    expectWhereNewRejected(
        "module M4166l; "
        "public record BoxObject4166l(O: Object, N: int); "
        "class Holder4166l<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166l; "
        "class Program4166l "
        "  public static method Main(): int "
        "    var b := Holder4166l<BoxObject4166l>.Make(); "
        "    return b.N; "
        "  end Main; "
        "end Program4166l;");
}

// (13) POSITIVE CONTROL: a record field of a builtin VALUE type
//      (`DateTime`, `isValueBuiltin=true` despite being a `Class`-kind
//      `TypeSymbol` internally, SemaContext.hpp's `builtinClassSym`) has
//      a real default and must NOT be flagged non-defaultable — proving
//      the `isValueBuiltin`-aware Class/Interface test
//      (`!t->declaration->isValueBuiltin`) does not over-reject. A flat
//      `categorize()`-based leaf test would have gotten this WRONG too
//      (categorize: Class ⇒ Reference, unconditionally, ignoring
//      isValueBuiltin) — this row would have FALSE-REJECTED under the
//      narrower leaf test if `DateTime` used it; the shared function is
//      what makes it correctly accept.
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_accepts_record_with_datetime_field_spec_14_2) {
    auto r = runAot(
        "module M4166m; "
        "public record BoxDate4166m(D: DateTime, N: int); "
        "class Holder4166m<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166m; "
        "class Program4166m "
        "  public static method Main(): int "
        "    var b := Holder4166m<BoxDate4166m>.Make(); "
        "    return b.N; "
        "  end Main; "
        "end Program4166m;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── (14)-(16) REGRESSION (review follow-up #3, §2.3/§14.2, v0.298): a
//        record field of an always-available BUILTIN collection type
//        (`Array<T>`, `Dictionary<K,V>`, `Set<T>` — TypeResolution_Names.cpp's
//        Slice-S24 population, represented as a Named TypeRef with a NULL
//        declaration, "no user TypeSymbol") must stay REJECTED under
//        `where T: new()`. `argSatisfiesNew`'s pre-hoist categorize()-based
//        leaf test caught this via an `astNode`/`builtinTypeArity`
//        fallback; the naive hoist to `typeHasNoDefaultWalk` (which ported
//        `defaultIllFormed`'s unconditional `if (!t->declaration) return
//        false;`) silently dropped it — a REGRESSION relative to
//        `argSatisfiesNew`'s own pre-hoist behavior, caught by review with
//        a live A/B: `record Box(A: Array<int>)` under `where T: new()`
//        REJECTS on parent `1e6d80f`, silently ACCEPTED (compiles clean,
//        runs) on the un-fixed hoist. This is the THIRD predicate this run
//        caught misclassifying this same ~50-builtin null-declaration
//        population — see issue #4229 (`refMentionsTypeVariable`,
//        `refMentionsAnyOf`) and the returned `fix #4199` for the other
//        three. ──────────────────────────────────────────────────────────
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_rejects_record_with_array_field_spec_14_2_2_3) {
    expectWhereNewRejected(
        "module M4166n; "
        "public record BoxArr4166n(A: Array<int>); "
        "class Holder4166n<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166n; "
        "class Program4166n "
        "  public static method Main(): int "
        "    var b := Holder4166n<BoxArr4166n>.Make(); "
        "    return 0; "
        "  end Main; "
        "end Program4166n;");
}

BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_rejects_record_with_dictionary_field_spec_14_2_2_3) {
    expectWhereNewRejected(
        "module M4166o; "
        "public record BoxDict4166o(D: Dictionary<string, int>); "
        "class Holder4166o<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166o; "
        "class Program4166o "
        "  public static method Main(): int "
        "    var b := Holder4166o<BoxDict4166o>.Make(); "
        "    return 0; "
        "  end Main; "
        "end Program4166o;");
}

BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_rejects_record_with_set_field_spec_14_2_2_3) {
    expectWhereNewRejected(
        "module M4166p; "
        "public record BoxSet4166p(S: Set<int>); "
        "class Holder4166p<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166p; "
        "class Program4166p "
        "  public static method Main(): int "
        "    var b := Holder4166p<BoxSet4166p>.Make(); "
        "    return 0; "
        "  end Main; "
        "end Program4166p;");
}

// ── (17) `default(...)`-TRIGGERED cycle guard (review-confirmed side
//        finding, §9/§17.11, v0.298): the row (8) self-referential-record
//        shape reached through the `default` operator directly instead of
//        `where T: new()` — the OTHER trigger the shared cycle guard
//        protects, and one review flagged as unpinned by any prior row.
//        Review measured `default(SelfSRD<Widget>)` as exit 139 (SIGSEGV)
//        on parent `1e6d80f` (`ExprTC::defaultIllFormed` had no cycle
//        guard at all, even before this fix's chain existed) — a
//        pre-existing, previously-untracked crash this fix's shared
//        function closes as a side effect of unifying with
//        `argSatisfiesNew`'s guard. Asserts the same CLEAN two-diagnostic
//        outcome as row (8), with the `default`-operator's OWN §17.11
//        wording (not the §14.2 `where T: new()` wording, since there is
//        no constraint check on this path — just the `default` operator's
//        own well-formedness check, ExprTypecheck_Exprs.cpp). ────────────
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           default_operator_self_referential_record_rejects_cleanly_no_crash_spec_9_17_11) {
    auto r = runAot(
        std::string("module M4166q; ") + kWidget4166 +
        "public record SelfSRD4166q<T>(N: SelfSRD4166q<int>, V: T); "
        "class Program4166q "
        "  public static method Main(): int "
        "    var x := default(SelfSRD4166q<Widget4166>); "
        "    return 0; "
        "  end Main; "
        "end Program4166q;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // §9 (checkRecordRestrictions / recordValueContainsSelf): the record's
    // own value-self-containment, rejected independently of this fix.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "cannot recursively contain itself by value"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9)"));
    // §17.11: the `default` operator's own well-formedness check,
    // correctly still evaluated — and still rejected, via field `V` —
    // after field `N`'s cycle is safely deferred rather than crashing.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`default(T)` is a compile error for a record any of whose "
           "fields is a non-nullable reference type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""17.11"));
}

// ── (18) `default(...)` Object regression control (review-suggested,
//        §17.11, v0.298): the row (12)/`Object`-carve-out fix's
//        `default(...)`-side consequence — REVIEW MEASURED this directly
//        (`default(BoxODP)`: ACCEPT on the pre-Object-fix hoist -> REJECT
//        after) and confirmed no existing test pinned the old ACCEPT, so
//        §7 does not require updating anything; this row pins the NEW,
//        spec-correct REJECT going forward. ──────────────────────────────
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           default_operator_rejects_record_with_object_field_spec_17_11) {
    auto r = runAot(
        "module M4166r; "
        "public record BoxObject4166r(O: Object, N: int); "
        "class Program4166r "
        "  public static method Main(): int "
        "    var x := default(BoxObject4166r); "
        "    return x.N; "
        "  end Main; "
        "end Program4166r;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`default(T)` is a compile error for a record any of whose "
           "fields is a non-nullable reference type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""17.11"));
}

// ── (19) POSITIVE CONTROL (review follow-up #3's own near-miss, §17.11,
//        v0.298): a record field of `Guid` — a VALUE builtin represented
//        with a NULL declaration (unlike `DateTime`/`Range`/etc., `Guid`
//        is never materialized via `ctx.builtinClassSym` at an ordinary
//        type-annotation site — TypeResolution_Names.cpp) — must stay
//        ACCEPTED under `where T: new()`. This is the row that would have
//        FAILED had the fix mirrored `categorize()`'s builtin fallback
//        wholesale instead of scoping it to the collection/`Task` family
//        by name; it is the same distinction
//        `tests/lit/builtin_closed_ctor_surface_accept_2994.bzl` pins for
//        the `default(...)` operator directly (`default(Guid)`,
//        `default(IntPtr)`). ─────────────────────────────────────────────
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_accepts_record_with_guid_field_spec_14_2) {
    auto r = runAot(
        "module M4166s; "
        "public record BoxGuid4166s(G: Guid, N: int); "
        "class Holder4166s<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166s; "
        "class Program4166s "
        "  public static method Main(): int "
        "    var b := Holder4166s<BoxGuid4166s>.Make(); "
        "    return b.N; "
        "  end Main; "
        "end Program4166s;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── (20)-(23) FFI-BOUNDARY CLASS REGRESSION (review-prompted investigation,
//        stdlib §10.1/§10.2/§10.3, v0.298): `CString`/`WString`/`RawBytes`
//        resolve through the IDENTICAL null-declaration path as the
//        collection family (TypeResolution_Names.cpp:390, one combined
//        arity check) and are classes with no public constructor (stdlib
//        §10) — non-defaultable, same as `Object`/`Array`. MEASURED
//        directly (not merely reasoned by analogy): `record Box(C:
//        CString, N: int)` under `where T: new()` compiled clean before
//        this addition; `default(Box(C: CString, ...))` compiled clean
//        and, when `.Length` was read on the zero-initialized field,
//        SIGSEGV'd (exit 139) — reproduced IDENTICALLY on this chain's
//        parent `1e6d80f` (git worktree
//        /var/tmp/bezel-scratch/r18/iso-cstring-parent, `bezelc --version`
//        confirmed `...-g1e6d80f`, clean tree), so this is a
//        pre-existing, previously-unmeasured defect, not something this
//        chain introduced. `WString`/`RawBytes` did NOT crash on the same
//        `.Length`-style probe (their layout tolerates a null internal
//        pointer for that accessor), but the §17.11 classification is
//        identical — asserted here as a clean compile-time REJECT for all
//        three, matching `CString`'s already-measured crash-preventing
//        behavior rather than relying on each one's own accessor
//        happening to fault. ─────────────────────────────────────────────
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_rejects_record_with_cstring_field_spec_10_1_14_2) {
    expectWhereNewRejected(
        "module M4166t; "
        "public record BoxCString4166t(C: CString, N: int); "
        "class Holder4166t<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166t; "
        "class Program4166t "
        "  public static method Main(): int "
        "    var b := Holder4166t<BoxCString4166t>.Make(); "
        "    return 0; "
        "  end Main; "
        "end Program4166t;");
}

// (21): the EXACT `default(...)` shape that SIGSEGV'd on the parent
// (measured above) — now asserts a clean compile-time rejection instead,
// with the `default` operator's own §17.11 wording.
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           default_operator_rejects_record_with_cstring_field_no_crash_spec_10_1_17_11) {
    auto r = runAot(
        "module M4166u; "
        "public record BoxCString4166u(C: CString, N: int); "
        "class Program4166u "
        "  public static method Main(): int "
        "    var x := default(BoxCString4166u); "
        "    return x.C.Length; "
        "  end Main; "
        "end Program4166u;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`default(T)` is a compile error for a record any of whose "
           "fields is a non-nullable reference type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""17.11"));
}

BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_rejects_record_with_wstring_field_spec_10_2_14_2) {
    expectWhereNewRejected(
        "module M4166v; "
        "public record BoxWString4166v(W: WString, N: int); "
        "class Holder4166v<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166v; "
        "class Program4166v "
        "  public static method Main(): int "
        "    var b := Holder4166v<BoxWString4166v>.Make(); "
        "    return 0; "
        "  end Main; "
        "end Program4166v;");
}

BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_rejects_record_with_rawbytes_field_spec_10_3_14_2) {
    expectWhereNewRejected(
        "module M4166w; "
        "public record BoxRawBytes4166w(R: RawBytes, N: int); "
        "class Holder4166w<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166w; "
        "class Program4166w "
        "  public static method Main(): int "
        "    var b := Holder4166w<BoxRawBytes4166w>.Make(); "
        "    return 0; "
        "  end Main; "
        "end Program4166w;");
}

// ── (24) POSITIVE CONTROL, second half of the near-miss (§32.5/§17.11,
//        v0.298): `IntPtr` — the FIFTH name `isFfiBoundaryNamedType`
//        groups with CString/WString/RawBytes for the FFI-compatibility
//        question (§32.5), but which is a VALUE type with a real default
//        (`default(IntPtr)` is pinned accepted by
//        tests/lit/builtin_closed_ctor_surface_accept_2994.bzl) — must
//        stay ACCEPTED here too. This is the row that would have FAILED
//        had the fix reused `isFfiBoundaryNamedType` wholesale instead of
//        writing its own narrower CString/WString/RawBytes-only check. ──
BEZEL_TEST(arg_satisfies_new_delegating_generic_record_4166,
           where_T_new_accepts_record_with_intptr_field_spec_14_2) {
    auto r = runAot(
        "module M4166x; "
        "public record BoxIntPtr4166x(P: IntPtr, N: int); "
        "class Holder4166x<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4166x; "
        "class Program4166x "
        "  public static method Main(): int "
        "    var b := Holder4166x<BoxIntPtr4166x>.Make(); "
        "    return b.N; "
        "  end Main; "
        "end Program4166x;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
