// fix #4002 (§14.2 / §17.11, v0.294): `Resolver::argSatisfiesNew`'s Record
// arm (src/sema/TypeResolution_Checks.cpp:7217+) — the sibling of
// `ExprTC::defaultIllFormed` covering the `where T: new()` constraint —
// enumerated a generic record's storage members but never substituted the
// record's OWN type parameter against the concrete instantiation. For
// `record Box<T>(V: T)`, `rsm.type` for `V` is the bare, declaration-less
// `T`; `categorize(ft)` on an unresolved bare parameter does not classify
// as `TypeCategory::Reference`, so `nonDefaultable` never got set and the
// constraint was satisfied regardless of what `T` was actually bound to.
//
// This is the SIBLING gap of fix #3986 in `ExprTC::defaultIllFormed`
// (ExprTypecheck_Exprs.cpp), never ported to this check. #3988 (the
// `recordStorageMembers` widening) touched this arm's field ENUMERATION
// but not its SUBSTITUTION — the lane that produced #3988 flagged this
// residual explicitly in its commit body but did not file it; this fix
// closes it.
//
// MEASURED on this fix's parent (origin/main `6460ea7`, the #3988-with-
// recordStorageMembers arm, before this fix):
//
//     module ArgSatisfiesNewGenericGap4;
//     public class WidgetANG4
//       public Count: int;
//       public constructor(c: int) Count := c; end constructor;
//     end WidgetANG4;
//     public record BoxANG4<T>(V: T);
//     class HolderANG4<T> where T: new()
//       public static method Make(): owned T => new T();
//     end HolderANG4;
//     class ProgANG4
//       public static method Main(): int
//         var b := HolderANG4<BoxANG4<WidgetANG4>>.Make();
//         return b.V.Count;
//       end Main;
//     end ProgANG4;
//
//   compiled at exit 0 with two internal diagnostics (#2477 codegen cannot
//   construct `new T()` in dead abstract template body, #2491 degraded
//   return), then SIGSEGV'd (exit 139) at runtime — `b.V` is a null
//   pointer manufactured by codegen's `new T()` record/field lowering,
//   which recursively default-constructs each field on the assumption
//   sema already rejected any field for which that has no valid value.
//
// THE FIX seeds a `common::RecordSubstEnv` from the record's own generic
// parameters against `arg`'s `genericArgs` (the symbol-side overload is
// correct for an imported record — `TypeSymbol::genericParams` is
// populated by ModuleLoader, unlike `decl`), and applies
// `common::resolveRecordFieldSubst(rsm.type, env).ty` per member before
// the `nullable`/`categorize` checks — the SAME primitives #3986 /
// #3988 already use in `ExprTC::defaultIllFormed` for
// identically-shaped generic substitution.
//
// SHAPE SWEEP (every test below is measured before/after the fix on
// this fix's parent — see the commit body for FAIL-WITHOUT / PASS-WITH
// counts):
//   - bare type param instantiated at a non-nullable class type
//       -> REJECT (§14.2)  — the defect;
//          `where_T_new_rejects_generic_record_bound_to_non_nullable_class`
//   - bare type param instantiated at a value type (`int`)
//       -> ACCEPT (constraint satisfied), runtime value 0
//          (`where_T_new_accepts_generic_record_bound_to_int`)
//   - bare type param instantiated at a nullable class type
//       -> ACCEPT, runtime value null
//          (`where_T_new_accepts_generic_record_bound_to_nullable_class`)
//   - NEGATIVE CONTROL: combined-form record whose body field (NOT
//       a generic param) holds a non-nullable class, through
//       `where T: new()` -> REJECT
//       (the #3988 sibling covered this with positional, plus the
//       combined-form body field; the negative control
//       `where_T_new_rejects_combined_form_body_field` keeps the
//       pre-existing #3988 coverage from regressing under the env
//       thread added by this fix)
//
// OUT OF SCOPE (deliberately deferred per the issue's own "Suggested
// fix shape" comment that calls out the #3995 composition as
// additional): a DELEGATING generic record
// (`record Outer<T>(In: Inner<T>)`) reached through `where T: new()`
// is a separate gap — `argSatisfiesNew` is a SHALLOW check that
// classifies the IMMEDIATE field's `Record` kind as `Value` per
// `categorize`, so a delegating shape never reaches an inner record's
// own generic parameter substitution even with this fix in place.
// The matching deep-recursive fix already exists for
// `ExprTC::defaultIllFormed` (#3995) and would need to be ported here
// in a follow-on issue; pre-existing #3995-like SIGSEGVs for this
// shape are not within #4002's substitution-only scope.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// Widget4002: non-nullable class with a primary-constructor parameter.
// Every REJECT test below uses this as the inner-leaf class type for
// the constraint check — the field/parameter that binds `T` of the
// record will end up referencing this class, and §17.11 says a record
// any of whose fields is a non-nullable reference type has no defined
// `default` (so `new T()` inside the holder cannot be satisfied).
const char* kWidget4002 =
    "public class Widget4002 "
    "  public Count: int; "
    "  public constructor(c: int) Count := c; end constructor; "
    "  public method Render(): int return Count; end Render; "
    "end Widget4002; ";

// Asserts a clean, real-location §14.2 rejection of `Holder<...>` over
// `program`, never a runtime crash (the exact class of defect: pre-fix
// this compiled clean and SIGSEGV'd at runtime).
void expectWhereNewRecordRejected(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r,
        "does not satisfy the constraint `where T: new()`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""14.2"));
}

}  // namespace

// ── (1) the issue's exact repro: `record Box4002<T>(V: T)` instantiated
//        at a non-nullable class type, used as the `T` of a
//        `where T: new()` holder that emits `new T()` ───────────────────
BEZEL_TEST(arg_satisfies_new_generic_record_subst_4002,
           where_T_new_rejects_generic_record_bound_to_non_nullable_class_spec_14_2) {
    expectWhereNewRecordRejected(
        std::string("module M4002; ") + kWidget4002 +
        "public record Box4002<T>(V: T); "
        "class Holder4002<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4002; "
        "class Program4002 "
        "  public static method Main(): int "
        "    var b := Holder4002<Box4002<Widget4002>>.Make(); "
        "    return b.V.Count; "
        "  end Main; "
        "end Program4002;");
}

// ── (2) positive control: bare type param instantiated at a value
//        type (`int`) — `new T()` for the inner record's `V: T` field
//        is `0`, the constraint IS satisfied, no diagnostic ────────────
BEZEL_TEST(arg_satisfies_new_generic_record_subst_4002,
           where_T_new_accepts_generic_record_bound_to_int_spec_14_2) {
    auto r = runAot(
        std::string("module M4002; ") +
        "public record Box4002<T>(V: T); "
        "class Holder4002<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4002; "
        "class Program4002 "
        "  public static method Main(): int "
        "    var b := Holder4002<Box4002<int>>.Make(); "
        "    return b.V; "
        "  end Main; "
        "end Program4002;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── (3) positive control: bare type param instantiated at a NULLABLE
//        class type — `new T()` for the inner record's `V: T` field is
//        a null reference, the constraint IS satisfied ─────────────────
BEZEL_TEST(arg_satisfies_new_generic_record_subst_4002,
           where_T_new_accepts_generic_record_bound_to_nullable_class_spec_14_2) {
    auto r = runAot(
        std::string("module M4002; ") + kWidget4002 +
        "public record Box4002<T>(V: T); "
        "class Holder4002<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4002; "
        "class Program4002 "
        "  public static method Main(): int "
        "    var b := Holder4002<Box4002<Widget4002?>>.Make(); "
        "    _ := b; "
        "    return 0; "
        "  end Main; "
        "end Program4002;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── (4) NEGATIVE CONTROL: keep #3988's existing
//        `where T: new()` combined-form-body-field coverage from
//        regressing under the env-thread added by this fix. A
//        NON-generic combined-form record whose body field is a
//        non-nullable class must STILL be rejected by the
//        `where T: new()` arm, identical to the (9) row in
//        `default_record_combined_form_body_field_3988_test.cpp`
//        (`where_new_constraint_rejects_combined_form_body_field`) ────
BEZEL_TEST(arg_satisfies_new_generic_record_subst_4002,
           where_T_new_rejects_combined_form_body_field_spec_14_2_3988_control) {
    expectWhereNewRecordRejected(
        std::string("module M4002; ") + kWidget4002 +
        "record Combo4002(X: int) "
        "  public Extra: Widget4002; "
        "end Combo4002; "
        "class Holder4002<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4002; "
        "class Program4002 "
        "  public static method Main(): int "
        "    var b := Holder4002<Combo4002>.Make(); "
        "    return b.Extra.Count; "
        "  end Main; "
        "end Program4002;");
}