// Regression tests for issue #5210 (follow-up to #5146) — spec §12.7 (v0.313),
// "Value-typed operands", and §3.6 rule 2.
//
// A type-parameter carrier `v: T` is decided per instantiation: `v as R`
// against a reference target `R` is rejected AT THE INSTANTIATION SITE where
// `T` is substituted by a value type, because a true answer would be an
// implicit box, which a type-test operand never performs. #5146 did this for
// METHOD-level parameters only; here a CLASS-level parameter (`class G<T>`
// instantiated `new G<int>()`) is rejected the same way (the sites are the
// `new G<…>()` construction and the generic-type static receiver, recorded
// keyed on the TypeDecl as the #5176 §19.3 join does).
//
// Oracle: reject rows assert compile failure AND the rule's own wording
// ("OPERAND against the reference target", §3.6 rule 2, §12.7); accept rows
// must compile and run with exit code 0.

#include "support/aot_support.hpp"  // runAot
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

bool rejectedByOperandRule(const std::string& program) {
    auto r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("`as` OPERAND against the reference target `IS`") !=
                std::string::npos &&
            d.message.find("rejected at the instantiation site") !=
                std::string::npos &&
            d.message.find("(\xC2\xA7""12.7)") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// The issue's own repro, class half: `G<int>.F` is rejected; `H.F<int>`
// (the method-level twin, #5146) is rejected as well.
BEZEL_TEST(class_generic_as_operand_instantiation_5210,
           issue_repro_class_and_method_carriers_rejected_spec_12_7) {
    const std::string prefix =
        "module M; interface IS end IS; "
        "class G<T> public method F(v: T): bool "
        "  var o := v as IS; return o <> null; end F; end G; "
        "class H public method F<T>(v: T): bool "
        "  var o := v as IS; return o <> null; end F; end H; ";
    // class-level only
    BEZEL_EXPECT_TRUE(rejectedByOperandRule(
        prefix +
        "class C public static method Main(): int "
        "  var g := new G<int>(); if g.F(4) then return 3; end; return 0; "
        "end Main; end C;"));
    // method-level control (#5146)
    BEZEL_EXPECT_TRUE(rejectedByOperandRule(
        prefix +
        "class C public static method Main(): int "
        "  var h := new H(); if h.F(4) then return 4; end; return 0; "
        "end Main; end C;"));
}

// A class generic instantiated at a RECORD (value type) is rejected.
BEZEL_TEST(class_generic_as_operand_instantiation_5210,
           class_generic_at_record_rejected_spec_12_7) {
    BEZEL_EXPECT_TRUE(rejectedByOperandRule(
        "module M; interface IS end IS; record P(X: int); "
        "class G<T> public method F(v: T): bool "
        "  var o := v as IS; return o <> null; end F; end G; "
        "class C public static method Main(): int "
        "  var g := new G<P>(); return 0; end Main; end C;"));
}

// Accept: the same class carrier instantiated at a REFERENCE type compiles
// and runs (the rule rejects only value substitutions).
BEZEL_TEST(class_generic_as_operand_instantiation_5210,
           class_generic_at_reference_type_accepted_spec_12_7) {
    auto r = runAot(
        "module M; interface IS end IS; "
        "class Impl : IS public constructor() end; end Impl; "
        "class G<T> public method F(v: T): bool "
        "  var o := v as IS; return o <> null; end F; end G; "
        "class C public static method Main(): int "
        "  var g := new G<Impl>(); var i := new Impl(); "
        "  g.F(i); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// fix #5210 residual (§12.7 v0.313, §3.6 rule 2): annotation-only
// class-level generic instantiation — `var g: G<int>` with no `new` —
// is seen only as a type annotation by Resolver, not by the `new`-expr
// handler, so recordDefaultObligationsForConstruction never fires and
// checkDeferredAsOperandInstantiations never sees the site.  The fix
// records the site explicitly in visitStmt (LocalDecl) and visitTopDecl
// (class extends clause) when the resolved type is a class-level generic
// instantiation with a value-substituted argument.
BEZEL_TEST(class_generic_as_operand_instantiation_5210,
           annotation_only_var_decl_rejected_spec_12_7) {
    BEZEL_EXPECT_TRUE(rejectedByOperandRule(
        "module M; interface IS end IS; "
        "class G<T> public method F(v: T): bool "
        "  var o := v as IS; return o <> null; end F; end G; "
        "class C public static method Main(): int "
        "  var g: G<int>; "
        "  return 0; end Main; end C;"));
}

// fix #5210 residual (§12.7 v0.313, §3.6 rule 2): annotation-only
// class-level generic instantiation — `class D : G<int>` (extends with
// a value-substituted generic) — is seen only as a type annotation by
// Resolver, not by the `new`-expr handler, so the instantiation site
// is not recorded.  The fix records the site in visitTopDecl when
// processing a class's extends clause.
BEZEL_TEST(class_generic_as_operand_instantiation_5210,
           annotation_only_class_extends_rejected_spec_12_7) {
    BEZEL_EXPECT_TRUE(rejectedByOperandRule(
        "module M; interface IS end IS; "
        "class G<T> public method F(v: T): bool "
        "  var o := v as IS; return o <> null; end F; end G; "
        "class D : G<int> end D; "
        "class C public static method Main(): int "
        "  return 0; end Main; end C;"));
}
