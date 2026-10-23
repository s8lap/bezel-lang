// Regression test for issue #7008 (§17.11 / §2.8.2 clause 2, v0.340).
//
// The library declares `Wrap<T>(): string => DefB<T>();` where `DefB<T>` takes
// an UNCONSTRAINED `default(T)`. Same-module, `U.Wrap<string>()` is rejected
// "type argument `string` is not defaultable (§17.11): the generic body
// passes `T` on to a generic whose chain ends in a `default(T)`". Across the
// module boundary it was ACCEPTED — the #6947 replay set holds only the
// DIRECT uses, so the importer saw no obligation for `Wrap` at all, and the
// body was monomorphized with `T = string`: the program SEGFAULTED.
//
// §2.8.2 clause 2: "An external instantiation behaves identically to a
// same-module instantiation of the same generic at the same arguments". The
// importer now closes the chain: a method whose body passes its
// own type parameter to a sibling whose body takes an unconstrained
// `default(T)` takes a `default(T)` obligation itself.
//
// The library is written TWICE, with the caller declared BEFORE its callee and
// AFTER it: the chain is closed by a fixpoint over the whole module, so the
// order in which the methods are declared must not decide the verdict.
//
// Every row runs under `runAotModulesChecked` (the strict AOT oracle:
// generated-code ASan + LSan, fork-isolated `Driver::compileFile`).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;

namespace {

// The library with the CALLEE declared first (the #7008 repro's shape).
const char* kLib =
    "module ULib;\n"
    "public class U\n"
    "  public static method DefB<T>(): string => $\"{default(T)}\";\n"
    "  public static method Wrap<T>(): string => DefB<T>();\n"
    "end U;\n"
    "public class Sq\n"
    "  public x: int;\n"
    "  public constructor(x: int) this.x := x; end constructor;\n"
    "end Sq;\n";

// The same library with the CALLER declared first: the chain is closed by a
// fixpoint over the module, so this order must give the same verdict.
const char* kLibReversed =
    "module ULib;\n"
    "public class U\n"
    "  public static method Wrap<T>(): string => DefB<T>();\n"
    "  public static method DefB<T>(): string => $\"{default(T)}\";\n"
    "end U;\n"
    "public class Sq\n"
    "  public x: int;\n"
    "  public constructor(x: int) this.x := x; end constructor;\n"
    "end Sq;\n";

// A chain of length two: `Wrap2<T>` passes `T` to `Mid<T>`, which passes it
// to `DefB<T>`. The fixpoint has to run to a fixed point, not one round.
const char* kLibChainOfTwo =
    "module ULib;\n"
    "public class U\n"
    "  public static method Wrap2<T>(): string => Mid<T>();\n"
    "  public static method Mid<T>(): string => DefB<T>();\n"
    "  public static method DefB<T>(): string => $\"{default(T)}\";\n"
    "end U;\n";

std::string mainOf(const std::string& call) {
    return "public class Program\n"
           "  public static method Main(): int\n"
           "    Console.WriteLine(" + call + ");\n"
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

JitResult runImported(const std::string& lib, const std::string& call) {
    std::string app = "module App;\nimport ULib;\n" + mainOf(call);
    return bezel::test::runAotModulesChecked({{"ULib", lib}, {"App", app}},
                                             "App");
}

JitResult runSameModule(const std::string& lib, const std::string& call) {
    // The library's own declarations, in ONE module, with the same call.
    std::string body = lib.substr(std::string("module ULib;\n").size());
    std::string one = "module One;\n" + body + mainOf(call);
    return bezel::test::runAotModulesChecked({{"One", one}}, "One");
}

}  // namespace

// The #7008 repro: the cross-module leg must be REJECTED, exactly as the
// same-module leg is.
BEZEL_TEST(xmod_generic_method_default_chain_7008,
           string_binding_rejected_as_not_defaultable_spec_17_11) {
    JitResult r = runImported(kLib, "U.Wrap<string>()");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "type argument `string` is not defaultable (\xC2\xA7""17.11)"));
}

// The same verdict when the CALLER is declared before its callee: the chain is
// closed by a fixpoint, not by the declaration order.
BEZEL_TEST(xmod_generic_method_default_chain_7008,
           reversed_declaration_order_rejected_spec_17_11) {
    JitResult r = runImported(kLibReversed, "U.Wrap<string>()");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "type argument `string` is not defaultable (\xC2\xA7""17.11)"));
}

// A chain of length two is closed too.
BEZEL_TEST(xmod_generic_method_default_chain_7008,
           chain_of_two_rejected_spec_17_11) {
    JitResult r = runImported(kLibChainOfTwo, "U.Wrap2<string>()");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "type argument `string` is not defaultable (\xC2\xA7""17.11)"));
}

// A CLASS type argument is not defaultable either (§17.11: a class has no
// well-defined zero value).
BEZEL_TEST(xmod_generic_method_default_chain_7008,
           class_binding_rejected_as_not_defaultable_spec_17_11) {
    JitResult r = runImported(kLib, "U.Wrap<Sq>()");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "is not defaultable (\xC2\xA7""17.11)"));
}

// Control: the SAME declaration in ONE module is rejected with the same text.
BEZEL_TEST(xmod_generic_method_default_chain_7008,
           same_module_control_rejected_spec_17_11) {
    JitResult r = runSameModule(kLib, "U.Wrap<string>()");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "type argument `string` is not defaultable (\xC2\xA7""17.11)"));
}

// Control: a DEFAULTABLE type argument still runs across the boundary — the
// chain is an obligation on the substitution, not a rejection of the method.
BEZEL_TEST(xmod_generic_method_default_chain_7008,
           int_binding_still_runs_spec_17_11) {
    JitResult r = runImported(kLib, "U.Wrap<int>()");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}
