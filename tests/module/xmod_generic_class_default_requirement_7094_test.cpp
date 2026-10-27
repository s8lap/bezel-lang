// Regression test for issue #7094 (§17.11 / §2.8.2 clause 2, v0.343).
//
// An IMPORTED generic CLASS's own §17.11 instantiation was not checked: the
// same-module typechecker rejects `Box<string>.Get()` ("type argument `string`
// is not defaultable (§17.11)"), while across the module boundary the
// importer ACCEPTED it and the program SEGFAULTED (exit 139).
//
// WHY the importer missed it. §17.11's obligation is a property of the
// generic's BODY: a body that takes `default(T)` is rejected at every
// non-defaultable substitution of T. The same-module typechecker discharges
// it by joining the body's `default(T)` use against the instantiation site —
// and for a CLASS-level type parameter BOTH are keyed on the class's
// `ast::TypeDecl`. An imported class has no `ast::TypeDecl` at all (#3141:
// ModuleLoader never assigns `Symbol::decl` for a type across the boundary),
// so the site no-opped and the join had nothing to fire on.
//
// THE FIX. The exporting module computes the obligation (it has the body) and
// ships it in the metadata (`MetaGenericParam::requiresWellDefinedDefault`);
// the loader publishes it on the imported TypeSymbol; the importing pass
// records the matching `default(T)` use against that same key, so the join
// reaches the same verdict the same-module typechecker reaches. §2.8.2 clause
// 2: "An external instantiation behaves identically to a same-module
// instantiation of the same generic at the same arguments."
//
// Every shape below is REJECTED cross-module AND same-module, and the
// cross-module leg must not run at all (the pre-fix leg SEGFAULTED).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;

namespace {

// The exporting module: a generic class whose static method's body takes
// `default(T)` of the CLASS's type parameter. §17.11 rejects every
// non-defaultable substitution of T at the instantiation site.
const char* kLib =
    "module BoxLib;\n"
    "public class Box<T>\n"
    "  public static method Get(): string => $\"{default(T)}\";\n"
    "  public constructor() end constructor;\n"
    "end Box;\n";

// The same library, but the `default(T)` is reached through a CHAIN: the
// class's own type parameter T is passed on to a sibling generic `DefB<T2>`
// whose body takes `default(T2)`. §17.11 reads such a chain as an obligation
// on the CLASS's T ("read through any chain of user declarations"), so
// `Box<string>.WrapC()` is rejected exactly like `Box<string>.Get()` is.
// `WrapC` itself names no type parameter of its own — the chain's owner is
// the CLASS.
const char* kLibChain =
    "module BoxChainLib;\n"
    "public class Box<T>\n"
    "  public static method DefB<T2>(): string => $\"{default(T2)}\";\n"
    "  public static method WrapC(): string => DefB<T>();\n"
    "  public constructor() end constructor;\n"
    "end Box;\n";

// The same library, but with a body that does NOT take `default(T)` — the
// control: nothing here requires a defaultable substitution.
const char* kLibPlain =
    "module BoxLibPlain;\n"
    "public class Box<T>\n"
    "  public static method Get(): string => \"plain\";\n"
    "  public constructor() end constructor;\n"
    "end Box;\n";

std::string mainOf(const std::string& body) {
    return "public class Program\n"
           "  public static method Main(): int\n" +
           body +
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

JitResult runImported(const std::string& lib,
                      const std::string& body,
                      const std::string& libName = "BoxLib") {
    std::string app = "module App;\nimport " + libName + ";\n" + mainOf(body);
    return bezel::test::runAotModulesChecked({{libName, lib}, {"App", app}},
                                              "App");
}

// The §17.11 diagnostic the same-module typechecker produces. Both spellings
// of the same rejection are pinned: the site and the wording.
const char* kNotDefaultable =
    "type argument `string` is not defaultable (\xC2\xA7""17.11)";

// A REJECT row, not a "did not crash" row: the program must be rejected with
// the §17.11 diagnostic, and it must never have run.
void expectRejectedAsNotDefaultable(const JitResult& r) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, kNotDefaultable));
    // The pre-fix leg SEGFAULTED at run time (exit 139). A crash is not an
    // acceptable substitute for the rejection.
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "terminated by signal"));
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "compiler CRASHED"));
}

}  // namespace

// ---------------------------------------------------------------------------
// The static-receiver spelling: `Box<string>.Get()`.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_class_default_requirement_7094,
           imported_class_static_receiver_rejected_not_defaultable_spec_17_11) {
    JitResult r = runImported(kLib, "    Console.WriteLine($\"{Box<string>.Get()}\");\n");
    expectRejectedAsNotDefaultable(r);
}

// ---------------------------------------------------------------------------
// The construction spelling: `new Box<string>()`.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_class_default_requirement_7094,
           imported_class_construction_rejected_not_defaultable_spec_17_11) {
    JitResult r = runImported(
        kLib,
        "    using var b := new Box<string>();\n"
        "    Console.WriteLine($\"{b.Get()}\");\n");
    expectRejectedAsNotDefaultable(r);
}

// ---------------------------------------------------------------------------
// The annotation spelling: `var b: Box<string> := new Box<string>();`.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_class_default_requirement_7094,
           imported_class_annotation_rejected_not_defaultable_spec_17_11) {
    JitResult r = runImported(
        kLib,
        "    var b: Box<string> := new Box<string>();\n"
        "    Console.WriteLine($\"{b.Get()}\");\n");
    expectRejectedAsNotDefaultable(r);
}

// ---------------------------------------------------------------------------
// The site spelled through the imported class from INSIDE another generic —
// the importer's own class-level chain. `Q<string>.M()` must be rejected for
// the same reason the same-module twin is.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_class_default_requirement_7094,
           imported_class_inside_local_generic_rejected_not_defaultable_spec_17_11) {
    std::string app =
        "module App;\n"
        "import BoxLib;\n"
        "public class Q<T>\n"
        "  public static method M(): string => Box<T>.Get();\n"
        "end Q;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine($\"{Q<string>.M()}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    JitResult r = bezel::test::runAotModulesChecked(
        {{"BoxLib", kLib}, {"App", app}}, "App");
    expectRejectedAsNotDefaultable(r);
}

// ---------------------------------------------------------------------------
// The CHAIN spelling: the class's own T reaches a `default(T)` through a
// sibling generic (`DefB<T2>`), so the CLASS carries the obligation. The
// chain's owner is the class, not a method — `WrapC` names no type parameter
// of its own.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_class_default_requirement_7094,
           imported_class_chain_rejected_not_defaultable_spec_17_11) {
    JitResult r = runImported(
        kLibChain, "    Console.WriteLine($\"{Box<string>.WrapC()}\");\n",
        "BoxChainLib");
    expectRejectedAsNotDefaultable(r);
}

// ---------------------------------------------------------------------------
// Control: the byte-identical SINGLE-MODULE program is rejected with the same
// §17.11 diagnostic (the wording the cross-module leg must agree with).
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_class_default_requirement_7094,
           same_module_twin_rejected_not_defaultable_spec_17_11) {
    std::string src =
        "module One;\n"
        "public class Box<T>\n"
        "  public static method Get(): string => $\"{default(T)}\";\n"
        "  public constructor() end constructor;\n"
        "end Box;\n" +
        mainOf("    Console.WriteLine($\"{Box<string>.Get()}\");\n");
    JitResult r = bezel::test::runAotModulesChecked({{"One", src}}, "One");
    expectRejectedAsNotDefaultable(r);
}

// ---------------------------------------------------------------------------
// Control: the byte-identical SINGLE-MODULE CHAIN program is rejected too,
// and in the CHAIN wording — the same-module typechecker names the derived
// obligation ("passes `T` on to a generic whose chain ends in a `default(T)`").
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_class_default_requirement_7094,
           same_module_chain_twin_rejected_not_defaultable_spec_17_11) {
    std::string src =
        "module One;\n"
        "public class Box<T>\n"
        "  public static method DefB<T2>(): string => $\"{default(T2)}\";\n"
        "  public static method WrapC(): string => DefB<T>();\n"
        "  public constructor() end constructor;\n"
        "end Box;\n" +
        mainOf("    Console.WriteLine($\"{Box<string>.WrapC()}\");\n");
    JitResult r = bezel::test::runAotModulesChecked({{"One", src}}, "One");
    expectRejectedAsNotDefaultable(r);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "passes `T` on to a generic whose chain ends in a `default(T)`"));
}

// ---------------------------------------------------------------------------
// Control: a VALUE-TYPE binding still runs across the boundary.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_class_default_requirement_7094,
           int_binding_still_runs_spec_17_11) {
    JitResult r = runImported(kLib, "    Console.WriteLine($\"{Box<int>.Get()}\");\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// ---------------------------------------------------------------------------
// Control: an imported generic class whose body takes NO `default(T)` is
// unaffected — a reference-typed instantiation still compiles and runs.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_generic_class_default_requirement_7094,
           class_without_default_requirement_still_runs_spec_17_11) {
    JitResult r = runImported(
        kLibPlain, "    Console.WriteLine($\"{Box<string>.Get()}\");\n",
        "BoxLibPlain");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("plain\n"));
}
