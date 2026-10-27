// Regression test for issue #7095 (§17.11 / §2.8.2 clause 2, v0.343).
//
// A `default(T)` chain that crosses a TYPE boundary inside the exporting
// module was not replayed across the module boundary: the same-module
// typechecker rejects `U.Wrap<string>()` ("type argument `string` is not
// defaultable (§17.11): the generic body passes `T` on to a generic whose
// chain ends in a `default(T)`"), while across the boundary the importer
// ACCEPTED it — and the program then SEGFAULTED (exit 139) in the non-generic
// owner shape, or printed `0` (the int placeholder the exported template was
// compiled against) and exited 0 in the generic-owner shape.
//
// WHY the importer missed it. §17.11's chain rule
// (`propagateDefaultTypeParamChains`) derives, at the DEFINITION, that a
// generic whose own type parameter is passed on to a generic whose chain ends
// in a `default(T)` takes `default` of that parameter too. The derived
// obligation is real, but it lived only in the exporting module's typecheck:
// nothing carried it across the boundary, so the importer judged the call
// against an empty obligation set and accepted it.
//
// THE FIX. The exporting module computes the obligation (it can see the whole
// chain, including across a type boundary) and ships it in the metadata
// (`MetaGenericParam::requiresWellDefinedDefault`); the loader publishes it
// on the imported symbol; the importing pass replays it at the instantiation
// site. §2.8.2 clause 2: "An external instantiation behaves identically to a
// same-module instantiation of the same generic at the same arguments."
//
// Every shape below is REJECTED cross-module AND same-module. The cross-module
// leg must not run at all — the pre-fix leg SEGFAULTED, or (worse) printed a
// wrong value and exited 0.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;

namespace {

const char* kNotDefaultable =
    "type argument `string` is not defaultable (\xC2\xA7""17.11)";

// The chain wording the same-module typechecker produces for a DERIVED
// obligation ("passes `T` on to a generic whose chain ends in a `default(T)`").
const char* kChainWording =
    "passes `T` on to a generic whose chain ends in a `default(T)`";

// A REJECT row, not a "did not crash" row: the program must be rejected with
// the §17.11 diagnostic, and it must never have run.
void expectRejectedAsNotDefaultable(const JitResult& r) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, kNotDefaultable));
    // The pre-fix leg SEGFAULTED at run time (exit 139) in the non-generic
    // owner shape. A crash is not an acceptable substitute for the rejection.
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "terminated by signal"));
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "compiler CRASHED"));
}

std::string mainOf(const std::string& call) {
    return std::string("public class Program\n"
                       "  public static method Main(): int\n"
                       "    Console.WriteLine($\"") +
           "{" + call + "}\");\n"
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

// Compile `lib` and `app` as two modules and return the result.
JitResult runTwo(const std::string& libName, const std::string& lib,
                 const std::string& app) {
    return bezel::test::runAotModulesChecked({{libName, lib}, {"App", app}},
                                              "App");
}

// The exporting library, in the shape the issue reports: the callee lives in
// ANOTHER type of the same module, so the chain crosses a TYPE boundary.
const char* kLibNonGenericOwner =
    "module ChainLib;\n"
    "public static class H\n"
    "  public static method DefB<T>(): string => $\"{default(T)}\";\n"
    "end H;\n"
    "public static class U\n"
    "  public static method Wrap<T>(): string => H.DefB<T>();\n"
    "end U;\n";

// The same chain, but the OWNER is a generic class: `U<T>.Wrap<Q>()`.
const char* kLibGenericOwner =
    "module ChainLib;\n"
    "public static class H\n"
    "  public static method DefB<T>(): string => $\"{default(T)}\";\n"
    "end H;\n"
    "public static class U<T>\n"
    "  public static method Wrap<Q>(): string => H.DefB<Q>();\n"
    "end U;\n";

// The chain crossing TWO type boundaries: U.Wrap -> H.Mid -> K.DefB.
const char* kLibTwoBoundaries =
    "module ChainLib;\n"
    "public static class K\n"
    "  public static method DefB<T>(): string => $\"{default(T)}\";\n"
    "end K;\n"
    "public static class H\n"
    "  public static method Mid<T>(): string => K.DefB<T>();\n"
    "end H;\n"
    "public static class U\n"
    "  public static method Wrap<T>(): string => H.Mid<T>();\n"
    "end U;\n";

// A METHOD boundary and then a type boundary: U.Wrap -> U.Inner -> H.DefB.
const char* kLibMethodThenType =
    "module ChainLib;\n"
    "public static class H\n"
    "  public static method DefB<T>(): string => $\"{default(T)}\";\n"
    "end H;\n"
    "public static class U\n"
    "  public static method Inner<T>(): string => H.DefB<T>();\n"
    "  public static method Wrap<T>(): string => Inner<T>();\n"
    "end U;\n";

// The owner's type parameter is INFERRED from an argument rather than
// spelled: `U.Wrap("hello")` binds T = string by §14.5's argument-driven
// inference, so the call is an instantiation site just as the explicit
// `U.Wrap<string>()` is.
const char* kLibInferred =
    "module ChainLib;\n"
    "public static class H\n"
    "  public static method DefB<T>(): string => $\"{default(T)}\";\n"
    "end H;\n"
    "public static class U\n"
    "  public static method Wrap<T>(x: T): string => H.DefB<T>();\n"
    "end U;\n";

// The callee lives in a BASE type of the exporting type.
const char* kLibBaseType =
    "module ChainLib;\n"
    "public class Base\n"
    "  public static method DefB<T>(): string => $\"{default(T)}\";\n"
    "end Base;\n"
    "public class Derived : Base\n"
    "  public static method Wrap<T>(): string => Base.DefB<T>();\n"
    "end Derived;\n";

// The callee lives in a BASE type of the exporting type and is reached
// through INHERITANCE — the unqualified call spelling.
const char* kLibBaseTypeInherited =
    "module ChainLib;\n"
    "public class Base\n"
    "  public static method DefB<T>(): string => $\"{default(T)}\";\n"
    "end Base;\n"
    "public class Derived : Base\n"
    "  public static method Wrap<T>(): string => DefB<T>();\n"
    "end Derived;\n";

// The callee is reached through an ALIAS of the exported type. A module-level
// alias is module-internal (§13.7), so the alias lives in the exporting
// module and the chain crosses the type boundary there.
const char* kLibAlias =
    "module ChainLib;\n"
    "public static class H\n"
    "  public static method DefB<T>(): string => $\"{default(T)}\";\n"
    "end H;\n"
    "type HH := H;\n"
    "public static class U\n"
    "  public static method Wrap<T>(): string => HH.DefB<T>();\n"
    "end U;\n";

}  // namespace

// ---------------------------------------------------------------------------
// The shape the issue reports: the owner is a NON-GENERIC class. Pre-fix this
// leg SEGFAULTED (exit 139) after being accepted.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_chain_default_requirement_7095,
           non_generic_owner_chain_rejected_not_defaultable_spec_17_11) {
    std::string app = "module App;\nimport ChainLib;\n" + mainOf("U.Wrap<string>()");
    JitResult r = runTwo("ChainLib", kLibNonGenericOwner, app);
    expectRejectedAsNotDefaultable(r);
}

// ---------------------------------------------------------------------------
// The owner is a GENERIC class: `U<int>.Wrap<string>()`. Pre-fix this leg was
// accepted and PRINTED `0` — the int placeholder the exported template was
// compiled against — and exited 0. A wrong answer with a zero exit code.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_chain_default_requirement_7095,
           generic_owner_chain_rejected_not_defaultable_spec_17_11) {
    std::string app =
        "module App;\nimport ChainLib;\n" + mainOf("U<int>.Wrap<string>()");
    JitResult r = runTwo("ChainLib", kLibGenericOwner, app);
    expectRejectedAsNotDefaultable(r);
}

// ---------------------------------------------------------------------------
// The chain crossing TWO type boundaries.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_chain_default_requirement_7095,
           two_type_boundaries_chain_rejected_not_defaultable_spec_17_11) {
    std::string app = "module App;\nimport ChainLib;\n" + mainOf("U.Wrap<string>()");
    JitResult r = runTwo("ChainLib", kLibTwoBoundaries, app);
    expectRejectedAsNotDefaultable(r);
}

// ---------------------------------------------------------------------------
// A METHOD boundary and then a type boundary.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_chain_default_requirement_7095,
           method_then_type_boundary_chain_rejected_not_defaultable_spec_17_11) {
    std::string app = "module App;\nimport ChainLib;\n" + mainOf("U.Wrap<string>()");
    JitResult r = runTwo("ChainLib", kLibMethodThenType, app);
    expectRejectedAsNotDefaultable(r);
}

// ---------------------------------------------------------------------------
// The callee lives in a BASE type of the exporting type.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_chain_default_requirement_7095,
           base_type_callee_chain_rejected_not_defaultable_spec_17_11) {
    std::string app =
        "module App;\nimport ChainLib;\n" + mainOf("Derived.Wrap<string>()");
    JitResult r = runTwo("ChainLib", kLibBaseType, app);
    expectRejectedAsNotDefaultable(r);
}

// ---------------------------------------------------------------------------
// The callee lives in a BASE type of the exporting type and is reached
// through INHERITANCE — the unqualified call spelling.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_chain_default_requirement_7095,
           inherited_base_type_callee_chain_rejected_not_defaultable_spec_17_11) {
    std::string app =
        "module App;\nimport ChainLib;\n" + mainOf("Derived.Wrap<string>()");
    JitResult r = runTwo("ChainLib", kLibBaseTypeInherited, app);
    expectRejectedAsNotDefaultable(r);
}

// ---------------------------------------------------------------------------
// The callee is reached through an ALIAS of the exported type.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_chain_default_requirement_7095,
           alias_callee_chain_rejected_not_defaultable_spec_17_11) {
    std::string app = "module App;\nimport ChainLib;\n" + mainOf("U.Wrap<string>()");
    JitResult r = runTwo("ChainLib", kLibAlias, app);
    expectRejectedAsNotDefaultable(r);
}

// ---------------------------------------------------------------------------
// The type argument is INFERRED from the call's argument (`U.Wrap("hello")`
// binds T = string by §14.5), not spelled. The call is an instantiation site
// all the same, so it is rejected for the same reason — and the diagnostic
// must NAME the offending type (`string`), not the generic placeholder.
//
// PRE-FIX this leg was rejected, but by a DIFFERENT rule: the importer's
// #1943 placeholder-layout check ("calling imported generic method `Wrap` with
// a type argument whose representation is not the compiled `int` (i32)
// placeholder"), because `string` is not the placeholder layout. The §17.11
// verdict the same-module typechecker reaches is what this row pins.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_chain_default_requirement_7095,
           argument_inferred_chain_rejected_not_defaultable_spec_17_11) {
    std::string app =
        "module App;\n"
        "import ChainLib;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var s: string := U.Wrap(\"hello\");\n"
        "    Console.WriteLine(s);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    JitResult r = runTwo("ChainLib", kLibInferred, app);
    expectRejectedAsNotDefaultable(r);
}

// ---------------------------------------------------------------------------
// Control: the byte-identical SINGLE-MODULE program is rejected with the
// §17.11 diagnostic, in the chain wording.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_chain_default_requirement_7095,
           same_module_twin_rejected_not_defaultable_spec_17_11) {
    std::string src =
        "module One;\n"
        "public static class H\n"
        "  public static method DefB<T>(): string => $\"{default(T)}\";\n"
        "end H;\n"
        "public static class U\n"
        "  public static method Wrap<T>(): string => H.DefB<T>();\n"
        "end U;\n" +
        mainOf("U.Wrap<string>()");
    JitResult r = bezel::test::runAotModulesChecked({{"One", src}}, "One");
    expectRejectedAsNotDefaultable(r);
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, kChainWording));
}

// ---------------------------------------------------------------------------
// Control: the byte-identical SINGLE-MODULE program with the INFERRED type
// argument is rejected too, in the chain wording.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_chain_default_requirement_7095,
           argument_inferred_same_module_twin_rejected_not_defaultable_spec_17_11) {
    std::string src =
        "module One;\n"
        "public static class H\n"
        "  public static method DefB<T>(): string => $\"{default(T)}\";\n"
        "end H;\n"
        "public static class U\n"
        "  public static method Wrap<T>(x: T): string => H.DefB<T>();\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var s: string := U.Wrap(\"hello\");\n"
        "    Console.WriteLine(s);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    JitResult r = bezel::test::runAotModulesChecked({{"One", src}}, "One");
    expectRejectedAsNotDefaultable(r);
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, kChainWording));
}

// ---------------------------------------------------------------------------
// Control: an INFERRED VALUE-TYPE binding still runs across the boundary —
// `U.Wrap(1)` binds T = int, which is defaultable.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_chain_default_requirement_7095,
           argument_inferred_int_binding_still_runs_spec_17_11) {
    std::string app =
        "module App;\nimport ChainLib;\n" + mainOf("U.Wrap(1)");
    JitResult r = runTwo("ChainLib", kLibInferred, app);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// ---------------------------------------------------------------------------
// Control: a VALUE-TYPE binding still runs across the boundary, in BOTH owner
// shapes.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_chain_default_requirement_7095,
           int_binding_still_runs_non_generic_owner_spec_17_11) {
    std::string app = "module App;\nimport ChainLib;\n" + mainOf("U.Wrap<int>()");
    JitResult r = runTwo("ChainLib", kLibNonGenericOwner, app);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

BEZEL_TEST(xmod_generic_chain_default_requirement_7095,
           int_binding_still_runs_generic_owner_spec_17_11) {
    std::string app =
        "module App;\nimport ChainLib;\n" + mainOf("U<int>.Wrap<int>()");
    JitResult r = runTwo("ChainLib", kLibGenericOwner, app);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}
