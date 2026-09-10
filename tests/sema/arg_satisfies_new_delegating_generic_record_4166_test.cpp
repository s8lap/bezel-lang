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
// THE FIX adds `Resolver::recordFieldTypeHasNoDefault`
// (TypeResolution_Checks.cpp), which recurses into a Record-kind field
// instead of stopping at `categorize()`, threading
// `resolveRecordFieldSubst`'s `Entry::env` into the inner frame — the SAME
// composition `ExprTC::defaultIllFormed` threads for the identically-shaped
// `default(T)` well-formedness question (fix #3995). `Resolver::run` (type
// resolution) executes before `ExprTC` exists (Driver.cpp: `resolveTypes`
// then `typecheckExpressions`), so this cannot simply call
// `defaultIllFormed` itself — the logic is ported, not shared, matching the
// established mirroring discipline already documented at every other
// `argSatisfiesNew` fix comment in this arm's history.
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
