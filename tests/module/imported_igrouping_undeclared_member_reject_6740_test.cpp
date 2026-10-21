// Cross-module coverage for #6740 (generalises #6714): a bare undeclared member
// on an IMPORTED `IGrouping<TKey, T>` (stdlib §5, §4.12, v0.340).
//
// ModuleLoader reconstructs a builtin generic imported from a library as a
// Named TypeRef with no declaration, no astNode and the name only in
// `importedName` (§2.5), so the in-module IGrouping arm (which reads the
// astNode name) never saw it: `g.Count` on a grouping from `Groups()` below
// was dropped behind a #3079 `internal:` warning. It is now rejected with the
// same "no member `<name>` on `IGrouping<TKey, T>`" diagnostic.
//
// No accept row: MEASURED, iterating an imported grouping at all
// (`for var g in G.Groups(a) do Console.WriteLine(g.Key);`) emits nothing at
// this commit (the library itself warns #2491 on `return a.GroupBy(...)`), a
// separate pre-existing defect, so there is no value to pin.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

constexpr const char* kLib6740 =
    "module Lib6740; "
    "public class G6740 "
    "  public static method Groups(a: Array<int>): "
    "IEnumerable<IGrouping<int, int>> "
    "    return a.GroupBy(lambda(x: int) => x); "
    "  end Groups; "
    "end G6740;";

std::string importer6740(const std::string& stmt) {
    return "module App6740; import Lib6740; "
           "class C6740 public static method Main(): int "
           "  var a: Array<int> := [1, 2, 2, 3]; "
           "  for var g in G6740.Groups(a) do " + stmt + " end; "
           "  return 0; end Main; end C6740;";
}

void expectRejected6740(const std::string& stmt, const std::string& name) {
    auto r = test::runAotModules(
        {{"Lib6740", kLib6740}, {"App6740", importer6740(stmt)}}, "App6740");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(test::diagnosticMessageContains(
        r, ("no member `" + name + "` on `IGrouping<TKey, T>`").c_str()));
}

}  // namespace

BEZEL_TEST(imported_igrouping_undeclared_member_reject_6740,
           imported_count_property_rejected_spec_5) {
    expectRejected6740("Console.WriteLine(g.Count);", "Count");
}

BEZEL_TEST(imported_igrouping_undeclared_member_reject_6740,
           imported_foo_rejected_spec_5) {
    expectRejected6740("Console.WriteLine(g.Foo);", "Foo");
}

BEZEL_TEST(imported_igrouping_undeclared_member_reject_6740,
           imported_size_rejected_spec_5) {
    expectRejected6740("var n: int := g.Size; Console.WriteLine(n);", "Size");
}

}  // namespace bezel::module
