// Regression test for issue #7054 (§29.3, §14.1, §2.8.2, v0.340).
//
// An imported generic ITERATOR yields a value built from its own type
// parameter: `Gen<T>(x: T): IEnumerable<Opt<T>>` doing `yield return
// Opt<T>.Some(x)`. The defining module compiles the method as the erased-
// placeholder template (§2.8.2), and in that template the payload-carrying
// choice construction installs the instantiation substitution
// (`choiceCtorSubst`, LlvmBackend_Calls2.cpp) and builds the SPECIALIZED
// `choice.Opt$spec0`, while the iterator's element type is the UNSPECIALIZED
// `choice.Opt`.
//
// The yield store is conditional on an exact LLVM type match, so the value was
// DROPPED: the consumer read the calloc-zeroed `current_val` slot and every
// payload read 0. MEASURED on the base: an imported generic iterator yielding
// `Opt<T>.Some(x)` at T = int printed `0` for 15, while the same-module
// instantiation printed 15. The sibling `No` case did store — it is
// payload-free, so it never took the specializing path.
//
// This is the SAME erased-template type-name mismatch fix #7049 fixed at the
// `return` site, at the `yield` store. The two structs are layout-identical,
// so the value is the same bits under another struct name; the fix reinterprets
// it (the same alloca round-trip `emitReturn` uses) before the store.
//
// §14.1: a call at explicit type arguments is an instantiation site. §2.8.2
// clause 2: "An external instantiation behaves identically to a same-module
// instantiation of the same generic at the same arguments." §29.3: `yield
// return` publishes the value to the consumer.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;

namespace {

const char* kLib7054 =
    "module Lib7054;\n"
    "public choice Opt<T> Some(v: T), No; end Opt;\n"
    "public choice Pair<T> Two(a: T, b: T), Nothing; end Pair;\n"
    "public class Api\n"
    "  public static method Gen<T>(x: T): IEnumerable<Opt<T>>\n"
    "    yield return Opt<T>.Some(x);\n"
    "    yield return Opt<T>.No;\n"
    "  end Gen;\n"
    "  public static method GenPair<T>(x: T): IEnumerable<Pair<T>>\n"
    "    yield return Pair<T>.Two(x, x);\n"
    "  end GenPair;\n"
    "  public static method GenPlain<T>(x: T): IEnumerable<T>\n"
    "    yield return x;\n"
    "  end GenPlain;\n"
    "end Api;\n";

// The same shapes in ONE module — the §2.8.2 clause-2 reference behaviour.
const char* kSameModule7054 =
    "module Same7054;\n"
    "public choice Opt<T> Some(v: T), No; end Opt;\n"
    "public class Api\n"
    "  public static method Gen<T>(x: T): IEnumerable<Opt<T>>\n"
    "    yield return Opt<T>.Some(x);\n"
    "    yield return Opt<T>.No;\n"
    "  end Gen;\n"
    "end Api;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    for var o in Api.Gen<int>(15) do\n"
    "      Console.WriteLine((case o of Some(var v) => $\"{v}\", No => \"no\"));\n"
    "    end;\n"
    "  end Main;\n"
    "end Program;\n";

std::string app7054(const std::string& body) {
    return "module App7054;\n"
           "import Lib7054;\n"
           "public class Program\n"
           "  public static method Main()\n"
           "    " + body + "\n"
           "  end Main;\n"
           "end Program;\n";
}

void expectPrints(const std::string& body, const char* want) {
    JitResult r = bezel::test::runAotModulesChecked(
        {ModuleSource{"Lib7054", kLib7054},
         ModuleSource{"App7054", app7054(body)}},
        "App7054");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

}  // namespace

// The issue's repro: the payload of the yielded choice. Pre-fix: `0`, not
// `15`.
BEZEL_TEST(xmod_generic_iterator_choice_yield_7054,
           issue_repro_yielded_choice_payload_prints_value_spec_29_3_14_1) {
    expectPrints("for var o in Api.Gen<int>(15) do "
                 "  Console.WriteLine((case o of Some(var v) => $\"{v}\", "
                 "                         No => \"no\")); "
                 "end;",
                 "15\nno\n");
}

// A two-field payload: BOTH fields must survive the reinterpret (pre-fix:
// the whole value was dropped, so both read 0).
BEZEL_TEST(xmod_generic_iterator_choice_yield_7054,
           two_field_yielded_payload_prints_both_spec_29_3_14_1) {
    expectPrints("for var o in Api.GenPair<int>(7) do "
                 "  Console.WriteLine((case o of Two(var a, var b) => "
                 "                         $\"{a},{b}\", Nothing => \"none\")); "
                 "end;",
                 "7,7\n");
}

// 200 iterations over the imported generic iterator: the sum of the payloads
// is 200 * 15 = 3000 (pre-fix: 0 — every payload was dropped), and LSan
// reports no leak.
BEZEL_TEST(xmod_generic_iterator_choice_yield_7054,
           loop_200_sums_yielded_payloads_no_leak_spec_29_3_14_1) {
    expectPrints("var total := 0; "
                 "for var i in 0..<200 do "
                 "  for var o in Api.Gen<int>(15) do "
                 "    total := total + (case o of Some(var v) => v, No => 0); "
                 "  end; "
                 "end; "
                 "Console.WriteLine(total);",
                 "3000\n");
}

// Control: the SAME-MODULE instantiation — the behaviour §2.8.2 clause 2
// requires the imported one to match. It was already correct; it pins the
// reference the imported row is measured against.
BEZEL_TEST(xmod_generic_iterator_choice_yield_7054,
           same_module_instantiation_prints_value_spec_2_8_2) {
    JitResult r = bezel::test::runAotChecked(kSameModule7054);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("15\nno\n"));
}

// Control: an imported generic iterator yielding a NON-choice element is
// unchanged — the reinterpret only fires for a layout-identical aggregate.
BEZEL_TEST(xmod_generic_iterator_choice_yield_7054,
           imported_generic_iterator_plain_element_unchanged_spec_29_3) {
    expectPrints("for var v in Api.GenPlain<int>(21) do "
                 "  Console.WriteLine(v); "
                 "end;",
                 "21\n");
}
