// Regression for #9290 — `SortedDictionary<K, V>.Single()` / `.Single(pred)`
// was typed as the value `V`, not as the entry `DictionaryEntry<K, V>`.
//
// THE ISSUE (stdlib §4.8 / §4.17 / §5, v0.359). A dictionary is an
// `ICollection<DictionaryEntry<K, V>>` (§4.8), and §5's `Single(): T` over it
// answers the entry, so `sd.Single().Key` / `.Value` are the entry's members.
// `First()` / `Last()` / `ElementAt()` and `Dictionary<K, V>.Single()` already
// typed that way; the SortedDictionary `Single` alone fell into the `V`
// catch-all (a carve-out kept while codegen had no `Single` arm, which #3761
// and #9261 supplied), so `.Key` was rejected as "no member on primitive type".
//
// THE ANSWERS (stdlib §5): `Single()` / `Single(pred)` raise
// `InvalidOperationException` when no element matches, and when more than one
// does.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& mainBody) {
    return "module M; class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// The issue's own repro: the result's `.Key` / `.Value` are the entry's.
BEZEL_TEST(sorted_dict_single_entry_9290, single_yields_the_entry_5_4_17) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var sd := new SortedDictionary<int, int>(); "
        "sd.Add(9, 90); "
        "var gs := sd.Single(); "
        "Console.WriteLine($\"{gs.Key} {gs.Value}\"); "
        "return 0; ")),
        "9 90");
}

// The predicate form answers the sole MATCHING entry, over several entries.
BEZEL_TEST(sorted_dict_single_entry_9290, single_pred_yields_the_matching_entry_5_4_17) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var sd := new SortedDictionary<int, int>(); "
        "sd.Add(3, 30); sd.Add(1, 10); sd.Add(2, 20); "
        "var gs := sd.Single(lambda(e) => e.Key = 2); "
        "Console.WriteLine($\"{gs.Key} {gs.Value}\"); "
        "return 0; ")),
        "2 20");
}

// A `string` key (the issue's second spelling), 200x under the checked oracle:
// the rebuilt key is released, and no throw path leaks.
BEZEL_TEST(sorted_dict_single_entry_9290, single_string_key_is_the_entry_and_does_not_leak_5) {
    JitResult r = runAotChecked(prog(
        "var total := 0; var i := 0; "
        "while i < 200 do "
        "  var sd := new SortedDictionary<string, int>(); "
        "  sd.Add(\"k\", 7); "
        "  var gs := sd.Single(); "
        "  total := total + gs.Key.Length + gs.Value; "
        "  i := i + 1; "
        "end while; "
        "Console.WriteLine($\"total={total}\"); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("total=1600") != std::string::npos);
}

// Several matches: `Single(pred)` raises InvalidOperationException (§5).
BEZEL_TEST(sorted_dict_single_entry_9290, single_pred_with_several_matches_throws_5) {
    JitResult r = runAotChecked(prog(
        "var caught := 0; var i := 0; "
        "while i < 200 do "
        "  var sd := new SortedDictionary<int, int>(); "
        "  sd.Add(1, 10); sd.Add(2, 20); "
        "  try var fe := sd.Single(lambda(e) => e.Value > 5); return fe.Key; "
        "  catch InvalidOperationException => caught := caught + 1; end; "
        "  i := i + 1; "
        "end while; "
        "Console.WriteLine($\"caught={caught}\"); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("caught=200") != std::string::npos);
}
