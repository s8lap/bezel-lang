// Codegen / collections — the `*OrNull` element-access terminals over a
// `Dictionary<K, V>` receiver did not lower at all (stdlib §4.8 / §5, spec
// §7.10). Issue #9234.
//
// THE DEFECT. `Dictionary<K, V>` is an `ICollection<DictionaryEntry<K, V>>`
// (stdlib §4.8), hence an `IEnumerable<DictionaryEntry<K, V>>`, so stdlib §5's
// element-access terminals apply to it and their element type is the synthetic
// `DictionaryEntry<K, V>`:
//
//   public method FirstOrNull(): T?;
//   public method LastOrNull(): T?;
//   public method SingleOrNull(): T?;
//
// stdlib §5 decides the answers: "`First`/`Last`/`Single` raise
// `InvalidOperationException` … the non-throwing form: the `*OrNull` variants,
// which return `null` instead" — so `FirstOrNull`/`LastOrNull` return the
// entry or null when the dictionary is EMPTY, and `SingleOrNull` returns the
// entry or null unless the dictionary holds EXACTLY ONE entry.
//
// The compiler TYPED those calls correctly (sema forms the `T?` at the
// dictionary's element type — the synthetic nullable `DictionaryEntry<K, V>`,
// the same shape §5's `ElementAtOrNull` returns), but CODEGEN had no arm for
// them over a dictionary receiver: the dictionary-terminal arm
// (LlvmBackend_Calls.cpp's `isDict` block) covered only `First` / `Last` /
// `Single` / `ElementAt` / `ElementAtOrNull`, and the generic LINQ path the
// three names fell through to does not lower a dictionary receiver either —
// so the call lowered to NOTHING and `lowerLocalDecl`'s `#2434` defense in
// depth zero-filled the local. The observable defect was a SILENT wrong
// value: a NON-EMPTY dictionary read as EMPTY (the program took the `null`
// arm and never saw the entry).
//
// THE FIX. The three names join the `isDict` block's terminal arm and reuse
// its whole machinery: the same count read (`bz_dict_count`, no allocation),
// the same snapshot (`bz_dict_keys_str`/`bz_dict_keys_gen` +
// `bz_dict_values_*`), the same key rebuild (a fresh, independently-owned
// `bz_string*` for a string key) and the same value decode. The `T?` result
// is the SAME tagged `{i8 hasValue; {K, V}}` struct §5's `ElementAtOrNull`
// already returns, built by the same present/null/merge shape — the null arm
// takes NO snapshot (so it mints nothing and leaks nothing) and reclaims an
// unbound receiver exactly as the present arm does.
//
// THE TWINS THE ISSUE SAYS ARE ALREADY CORRECT keep their behaviour: the
// non-nullable `First()` / `Last()` / `Single()` over a dictionary (they
// lower today) and the array family's own `*OrNull` terminals.
//
// THE SCOPE OF THE OBSERVABLE DEFECT — MEASURED, and worth recording: the
// base's zero-degraded local happens to carry the RIGHT answer for an EMPTY
// dictionary (a zeroed `DictionaryEntry?` has no value, so `= null` is true
// and the program prints "none" — which is also the correct answer). The
// wrong-value defect is therefore observable only where the correct answer IS
// an entry: a NON-EMPTY dictionary for `FirstOrNull`/`LastOrNull`, and a
// dictionary holding EXACTLY ONE entry for `SingleOrNull`. The empty-source
// rows are consequently GREEN on the base and are not gtest rows here (a
// base-green row pins nothing, CLAUDE.md *Test and oracle rules*); they are
// left to the conformance corpus with their deciding spec sentence.
//
// Spec: stdlib §4.8 *`Dictionary<K, V>`*, stdlib §5 *`IEnumerable<T>`* (the
// `*OrNull` tier), spec §7.10 *Nullability* (a `T?` distinguishes "no value"
// from "a value"), spec §28.9 *Runtime-managed types* (the rebuilt `string`
// key is released at the entry local's scope end). Spec revision v0.359.
//
// The oracle: `runAotChecked` for the string-key rows (the compiler mints a
// fresh key `bz_string*` there, so the row also proves it is released — a
// leak flips `r.ran`), `runAot` for the `K = int` rows (no key is ever
// rebuilt there — the key is decoded by value, so there is nothing to
// release). The leak row loops 200 times: a one-shot repro hides under the
// sanitizer's noise.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The issue's own repro, VERBATIM: a `Dictionary<string, int>` holding ONE
// entry, and the three `*OrNull` terminals over it.
const char* kIssueRepro =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    using var d := new Dictionary<string, int>();\n"
    "    d[\"a\"] := 1;\n"
    "    var k1 := d.FirstOrNull();\n"
    "    if k1 = null then Console.WriteLine(\"first none\"); else "
    "Console.WriteLine($\"first {k1.Key}\"); end;\n"
    "    var k2 := d.LastOrNull();\n"
    "    if k2 = null then Console.WriteLine(\"last none\"); else "
    "Console.WriteLine($\"last {k2.Key}\"); end;\n"
    "    var k3 := d.SingleOrNull();\n"
    "    if k3 = null then Console.WriteLine(\"single none\"); else "
    "Console.WriteLine($\"single {k3.Key}\"); end;\n"
    "  end Main;\n"
    "end Program;\n";

// The `K = int` twin of the issue's repro: an `int` key is decoded by value
// (no key string is ever rebuilt), so the answer is the entry's int key.
const char* kIssueReproIntKeys =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    using var d := new Dictionary<int, int>();\n"
    "    d[7] := 1;\n"
    "    var k1 := d.FirstOrNull();\n"
    "    if k1 = null then Console.WriteLine(\"first none\"); else "
    "Console.WriteLine($\"first {k1.Key}\"); end;\n"
    "    var k2 := d.LastOrNull();\n"
    "    if k2 = null then Console.WriteLine(\"last none\"); else "
    "Console.WriteLine($\"last {k2.Key}\"); end;\n"
    "    var k3 := d.SingleOrNull();\n"
    "    if k3 = null then Console.WriteLine(\"single none\"); else "
    "Console.WriteLine($\"single {k3.Key}\"); end;\n"
    "  end Main;\n"
    "end Program;\n";

// The leak row: 200 iterations, each minting one rebuilt key through the
// terminal materializer. On the base the local is zero-filled (so the
// printed key is missing); with the fix the key is printed AND released at
// the entry local's scope end (a leak would flip `r.ran`).
const char* kLeakLoop =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      using var d := new Dictionary<string, int>();\n"
    "      d[$\"a{i}\"] := 1;\n"
    "      var kv := d.FirstOrNull();\n"
    "      if kv = null then Console.WriteLine(\"none\"); else "
    "Console.WriteLine($\"{kv.Key}\"); end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "  end Main;\n"
    "end Program;\n";

// The 200 printed keys, one per iteration.
std::string expectedPerIteration() {
    std::string out;
    for (int i = 0; i < 200; ++i) out += "a" + std::to_string(i) + "\n";
    return out;
}

}  // namespace

BEZEL_TEST(ownership_dict_ornull_terminals_9234,
           the_firstornull_returns_the_entry_of_a_non_empty_dictionary_spec_5) {
    JitResult r = runAotChecked(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);          // no leak, no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The whole repro's stdout: the first entry's key is "a" — on the base
    // the zero-degraded local read as null and this printed "first none".
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("first a\nlast a\nsingle a\n"));
}

BEZEL_TEST(ownership_dict_ornull_terminals_9234,
           the_lastornull_returns_the_entry_of_a_non_empty_dictionary_spec_5) {
    JitResult r = runAotChecked(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("first a\nlast a\nsingle a\n"));
}

BEZEL_TEST(ownership_dict_ornull_terminals_9234,
           the_singleornull_returns_the_entry_of_a_non_empty_dictionary_spec_5) {
    JitResult r = runAotChecked(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("first a\nlast a\nsingle a\n"));
}

BEZEL_TEST(ownership_dict_ornull_terminals_9234,
           the_firstornull_returns_the_entry_of_a_non_empty_dictionary_of_int_keys_spec_5) {
    JitResult r = runAot(kIssueReproIntKeys);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("first 7\nlast 7\nsingle 7\n"));
}

BEZEL_TEST(ownership_dict_ornull_terminals_9234,
           the_lastornull_returns_the_entry_of_a_non_empty_dictionary_of_int_keys_spec_5) {
    JitResult r = runAot(kIssueReproIntKeys);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("first 7\nlast 7\nsingle 7\n"));
}

BEZEL_TEST(ownership_dict_ornull_terminals_9234,
           the_singleornull_returns_the_entry_of_a_non_empty_dictionary_of_int_keys_spec_5) {
    JitResult r = runAot(kIssueReproIntKeys);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("first 7\nlast 7\nsingle 7\n"));
}

BEZEL_TEST(ownership_dict_ornull_terminals_9234,
           the_rebuilt_key_of_an_ornull_entry_is_released_spec_28_9) {
    JitResult r = runAotChecked(kLeakLoop);
    BEZEL_EXPECT_TRUE(r.ran);          // no leak (the bug), no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedPerIteration());
}
