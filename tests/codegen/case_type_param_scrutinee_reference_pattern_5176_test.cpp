// fix #5176 / §19.3 (v0.310, ruling #5156): a generic body decides the
// admissibility of a `case` arm against a reference-typed pattern PER
// INSTANTIATION. At a value substitution the choices are:
//   * a bare `R` arm or a class-headed destructure is ADMITTED and at runtime
//     never selected (the runtime scrutinee is not an R value);
//   * a typed binding `var r: R` is REJECTED at the instantiation site
//     (no implicit box is performed at pattern position — pattern position
//     matches by value, so a value scrutinee can never produce an R, and
//     boxing the scrutinee would silently change its identity);
//   * a destructure's slot binding is a MEMBER READ of the head's slot
//     (the head's field), not a binding of the scrutinee, and is admitted
//     without rejection at the instantiation site.
//
// Per §3.6 rule 2 the per-instantiation decision is a Pass-2 join over a
// sema recorder: the body records one entry per (owner, type-parameter,
// binding-pattern site) seen; the join walks the method's instantiation
// sites and emits the §19.3 diagnostic at the call site (where the
// substituted type is known) rather than at the binding pattern (where
// the abstract parameter is what the typechecker sees).
// The same predicate is reused by the §17.4 `is`-/`as`-operator recorder
// (#4866), so a single §3.6 join covers both forms.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §19.3 v0.310 (#5156, #5176): a bare interface arm `case v of IShape => 1,
// _ => 9` in a generic body is ADMITTED at the value substitution T = int.
// The arm never matches at runtime (an int is not an IShape), so the
// wildcard arm fires and the result is 9. Without #5176 the arm is also
// admitted (the pre-existing §19.3 behavior), so this row is a control that
// the per-instantiation decision for the bare form did not regress.
BEZEL_TEST(case_type_param_scrutinee_reference_pattern_5176,
           bare_interface_int_admitted_never_selected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IShape method M(): int; end IShape; "
        "class Program "
        "  public static method Pick<T>(v: T): int "
        "    return (case v of IShape => 1, _ => 9); "
        "  end Pick; "
        "  public static method Main(): int "
        "    return Program.Pick<int>(5); "
        "  end Main; end Program;"), 9);
}

// §19.3 v0.310 (#5156, #5176): a typed-binding arm `case v of var x:
// IShape => 1, _ => 9` in a generic body is REJECTED at the value
// instantiation site T = int. The §3.6 join reports the §19.3 diagnostic
// at the call site (Program.Pick<int>), not at the binding pattern.
// Without #5176 the binding arm is silently admitted, and the program
// compiles — so this row fails on pre-fix code (compile succeeded) and
// passes on the fix (compile rejected with the §19.3 diagnostic).
BEZEL_TEST(case_type_param_scrutinee_reference_pattern_5176,
           typed_binding_interface_int_rejected_at_site) {
    auto r = runAot(
        "module M; "
        "interface IShape method M(): int; end IShape; "
        "class Program "
        "  public static method Pick<T>(v: T): int "
        "    return (case v of var x: IShape => 1, _ => 9); "
        "  end Pick; "
        "  public static method Main(): int "
        "    return Program.Pick<int>(5); "
        "  end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "reference-typed binding"));
    // The diagnostic is anchored at the instantiation site (Program.Pick<int>),
    // per §3.6 rule 2 / §19.3, so the recorded use-line refers back to the
    // binding pattern in the generic body.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "line "));
}

// §19.3 v0.310 (#5156, #5176): a bare class arm `case v of C => 1, _ => 9`
// in a generic body is ADMITTED at the value substitution T = int. The arm
// never matches at runtime (an int is not a C), so the wildcard arm fires
// and the result is 9. Symmetric to the interface row above, but exercises
// the class-typed arm path through the same per-instantiation join.
BEZEL_TEST(case_type_param_scrutinee_reference_pattern_5176,
           bare_class_int_admitted_never_selected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public method M(): int return 0; end M; end C; "
        "class Program "
        "  public static method Pick<T>(v: T): int "
        "    return (case v of C => 1, _ => 9); "
        "  end Pick; "
        "  public static method Main(): int "
        "    return Program.Pick<int>(5); "
        "  end Main; end Program;"), 9);
}

// §19.3 v0.310 (#5156, #5176): a RECORD-headed destructure with a slot
// binding `case v of Rec(var x) => x, _ => 9` in a generic body is ADMITTED
// at the value substitution T = int — the slot binding is a MEMBER READ of
// the head's slot, not a binding of the scrutinee, so no box is required.
// The arm never matches at runtime (an int is not a Rec), so the wildcard
// fires and the result is 9. This is the "not rejected" half of the
// per-instantiation decision; the rejection half is the typed-binding row
// above.
BEZEL_TEST(case_type_param_scrutinee_reference_pattern_5176,
           record_destructure_slot_binding_int_admitted) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Rec public property N: int; end Rec; "
        "class Program "
        "  public static method Pick<T>(v: T): int "
        "    return (case v of Rec(var x) => x, _ => 9); "
        "  end Pick; "
        "  public static method Main(): int "
        "    return Program.Pick<int>(5); "
        "  end Main; end Program;"), 9);
}
