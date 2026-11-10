// Regression pin for #8356 (spec §5.1, stdlib §4.19, v0.357): an UNANNOTATED
// `using var g := FrozenArray<int>.From(a);` passed to an `IEvent<int>`
// parameter must be rejected - FrozenArray<T> converts only to
// IEnumerable<T> (§5.1: one class-to-interface conversion, by declaration).
// Sibling of #8258 (annotated form, c775e0777).
//
// ORACLE: `runAot`; asserts the SPECIFIC §5.1 diagnostic.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

BEZEL_TEST(frozenarray_unannotated_ievent_8356, unannotated_frozenarray_to_ievent_rejected_5_1) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method N(x: IEvent<int>): int => 0;\n"
        "  public static method Main(): int\n"
        "    using var a: Array<int> := [1, 2, 3];\n"
        "    using var g := FrozenArray<int>.From(a);\n"
        "    Console.WriteLine(N(g));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}
