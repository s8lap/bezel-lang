// Codegen / collections — the §5 PREDICATE-form element-access terminals
// over a `Dictionary<K, V>` receiver did not lower (stdlib §4.8 / §5, spec
// §7.10 / §28.9). Issue #9261.
//
// THE DEFECT. `Dictionary<K, V>` is an `ICollection<DictionaryEntry<K, V>>`
// (stdlib §4.8), hence an `IEnumerable<DictionaryEntry<K, V>>`, so stdlib §5's
// element-access terminals apply to it and their element type is the
// synthetic `DictionaryEntry<K, V>`:
//
//   public method First(predicate: (T) -> bool): T;
//   public method FirstOrNull(predicate: (T) -> bool): T?;
//   public method Last(predicate: (T) -> bool): T;
//   public method LastOrNull(predicate: (T) -> bool): T?;
//   public method Single(predicate: (T) -> bool): T;
//   public method SingleOrNull(predicate: (T) -> bool): T?;
//
// so the predicate's parameter is an ENTRY (`e.Key` / `e.Value`), not the key.
//
// TWO gaps, one issue.
//
// (1) CODEGEN had no arm for the six predicate forms over a dictionary
// receiver: the dictionary-terminal arm (LlvmBackend_Calls.cpp's `isDict`
// block) covered only the NO-ARGUMENT terminals, and the generic LINQ path
// the six names fell through to does not lower a dictionary receiver either
// — so the call lowered to NOTHING and `lowerLocalDecl`'s `#2434` defense in
// depth zero-filled the local. The observable defect was a SEGFAULT in the
// three non-nullable forms (the zero-filled entry's `Key` is a null pointer)
// and a SILENT wrong value in the three `*OrNull` forms (a non-empty
// dictionary read as EMPTY).
//
// (2) SEMA left the PREDICATE'S PARAMETER UNTYPED for the three `*OrNull`
// forms: the lambda-parameter-seeding table (`BuiltinRegistry.cpp`'s
// `kLinqElementLambdaOperatorNames` — the gate `linqLambdaElementType`
// reads) listed `First` / `Last` / `Single` but not their `*OrNull`
// siblings, so `d.FirstOrNull(lambda(e) => e.Value > 5)`'s `e` had no type.
// The predicate's BODY could then not be typed at all (`e.Value` is a member
// access on an untyped `e`), and codegen emitted the closure with a constant
// `ret i1 false` — so no entry ever matched and the `*OrNull` forms read as
// EMPTY even after (1) was closed. The array family survived the same gap
// only by accident: `arr.FirstOrNull(lambda(x) => x > 5)`'s untyped `x`
// happened to infer `int` from the literal.
//
// THE ANSWERS (stdlib §5). "`First(pred)` and `Last(pred)` yield the first
// and last matching element, and `Single(pred)` the sole matching element …
// `First(pred)`/`Last(pred)`/`Single(pred)` raise
// `InvalidOperationException` when no element matches (and `Single(pred)`
// also when more than one matches), while `FirstOrNull(pred)`/
// `LastOrNull(pred)`/`SingleOrNull(pred)` return `null` when none matches
// (`SingleOrNull(pred)` still raises when more than one matches)." So the
// scan COUNTS the matches and the count decides every outcome.
//
// THE TWINS THE ISSUE SAYS ARE ALREADY CORRECT keep their behaviour: the
// no-argument terminals over a dictionary (`First()` / `Last()` / `Single()`
// and the `*OrNull` tier #9234 closed) and the array family's own predicate
// terminals. The two green controls below pin exactly those.
//
// THE SCOPE OF THE OBSERVABLE DEFECT — MEASURED, and worth recording: the
// base's zero-degraded local happens to carry the RIGHT answer where NO
// element matches (a zeroed `DictionaryEntry?` has no value, so `= null` is
// true and the program prints "none" — which is also the correct answer).
// The wrong-value defect is therefore observable only where the correct
// answer IS an entry: a NON-EMPTY dictionary whose predicate matches, for
// the `*OrNull` forms. The no-match rows are consequently GREEN on the base
// and are not gtest rows here (a base-green row pins nothing, CLAUDE.md
// *Test and oracle rules*); they are left to the conformance corpus with
// their deciding spec sentence.
//
// Spec: stdlib §4.8 *`Dictionary<K, V>`*, stdlib §5 *`IEnumerable<T>`* (the
// element-access tier, predicate overloads and the `*OrNull` tier), spec §7.10
// *Nullability* (a `T?` distinguishes "no value" from "a value"), spec §28.9
// *Runtime-managed types* (the rebuilt `string` key is released at the entry
// local's scope end). Spec revision v0.359.
//
// The oracle: `runAotChecked` for the string-key rows (the compiler mints a
// fresh key `bz_string*` there, so the row also proves it is released — a
// leak flips `r.ran`), `runAot` for the `K = int` row (no key is ever
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

// The issue's own repro, VERBATIM: `First(predicate)` over a
// `Dictionary<string, int>` holding two entries, one of which matches.
const char* kIssueRepro =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    using var d := new Dictionary<string, int>();\n"
    "    d[\"a\"] := 1;\n"
    "    d[\"b\"] := 9;\n"
    "    var v := d.First(lambda(e) => e.Value > 5);\n"
    "    Console.WriteLine($\"first {v.Key}\");\n"
    "  end Main;\n"
    "end Program;\n";

// The `Last(predicate)` sibling of the issue's repro — the last MATCHING
// entry (the dictionary is walked in §4.17's key order).
const char* kLastRepro =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    using var d := new Dictionary<string, int>();\n"
    "    d[\"a\"] := 1;\n"
    "    d[\"b\"] := 9;\n"
    "    var v := d.Last(lambda(e) => e.Value > 5);\n"
    "    Console.WriteLine($\"last {v.Key}\");\n"
    "  end Main;\n"
    "end Program;\n";

// The `Single(predicate)` sibling — the SOLE matching entry.
const char* kSingleRepro =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    using var d := new Dictionary<string, int>();\n"
    "    d[\"a\"] := 1;\n"
    "    var v := d.Single(lambda(e) => e.Value > 0);\n"
    "    Console.WriteLine($\"single {v.Key}\");\n"
    "  end Main;\n"
    "end Program;\n";

// The three `*OrNull` PREDICATE forms over a dictionary holding a MATCHING
// entry — the shape the base read as EMPTY.
const char* kOrNullRepro =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    using var d := new Dictionary<string, int>();\n"
    "    d[\"a\"] := 1;\n"
    "    d[\"b\"] := 9;\n"
    "    var v := d.FirstOrNull(lambda(e) => e.Value > 5);\n"
    "    if v = null then Console.WriteLine(\"first none\"); else "
    "Console.WriteLine($\"first {v.Key} {v.Value}\"); end;\n"
    "    var w := d.LastOrNull(lambda(e) => e.Value > 5);\n"
    "    if w = null then Console.WriteLine(\"last none\"); else "
    "Console.WriteLine($\"last {w.Key} {w.Value}\"); end;\n"
    "    var s := d.SingleOrNull(lambda(e) => e.Value > 5);\n"
    "    if s = null then Console.WriteLine(\"single none\"); else "
    "Console.WriteLine($\"single {s.Key} {s.Value}\"); end;\n"
    "  end Main;\n"
    "end Program;\n";

// stdlib §5: "`SingleOrNull(pred)` still raises when more than one
// matches" — the signature's own comment says the same ("null if none;
// raises if more than one"). Two entries match here.
const char* kSingleOrNullRaises =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    using var d := new Dictionary<string, int>();\n"
    "    d[\"a\"] := 1;\n"
    "    d[\"b\"] := 9;\n"
    "    try\n"
    "      var v := d.SingleOrNull(lambda(e) => e.Value > 0);\n"
    "      if v = null then Console.WriteLine(\"no raise\"); else "
    "Console.WriteLine($\"no raise {v.Key}\"); end;\n"
    "    catch ex: InvalidOperationException =>\n"
    "      Console.WriteLine(\"raised\");\n"
    "    end;\n"
    "  end Main;\n"
    "end Program;\n";

// The `K = int` twin: an int key is decoded by value (no key string is ever
// rebuilt), so the answer is the entry's int key.
const char* kIntKeyRepro =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    using var d := new Dictionary<int, int>();\n"
    "    d[7] := 1;\n"
    "    d[9] := 9;\n"
    "    var v := d.First(lambda(e) => e.Value > 5);\n"
    "    Console.WriteLine($\"first {v.Key} {v.Value}\");\n"
    "  end Main;\n"
    "end Program;\n";

// The leak row: 200 iterations, each rebuilding a string key through the
// terminal's key decode (the terminal's own +1, released at the entry
// local's scope end).
const char* kLeakLoop =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      using var d := new Dictionary<string, int>();\n"
    "      d[$\"a{i}\"] := 1;\n"
    "      d[$\"b{i}\"] := 9;\n"
    "      var kv := d.First(lambda(e) => e.Value > 5);\n"
    "      Console.WriteLine(kv.Key);\n"
    "      i := i + 1;\n"
    "    end;\n"
    "  end Main;\n"
    "end Program;\n";

// The 200 printed keys, one per iteration.
std::string expectedPerIteration() {
    std::string out;
    for (int i = 0; i < 200; ++i) out += "b" + std::to_string(i) + "\n";
    return out;
}

// GREEN CONTROL 1 — the no-argument `*OrNull` tier over the SAME receiver
// (#9234's fix): a dictionary holding TWO entries answers null (the entry
// is returned only when the dictionary holds EXACTLY ONE). This row is green
// on the base and must stay green: a wrong fix that let the predicate tier
// change the no-argument tier's answer (or that made the `*OrNull` tier
// answer the FIRST entry) turns it red.
const char* kNoArgOrNullControl =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    using var d := new Dictionary<string, int>();\n"
    "    d[\"a\"] := 1;\n"
    "    d[\"b\"] := 9;\n"
    "    var n := d.SingleOrNull();\n"
    "    if n = null then Console.WriteLine(\"noarg none\"); else "
    "Console.WriteLine($\"noarg {n.Key}\"); end;\n"
    "  end Main;\n"
    "end Program;\n";

// GREEN CONTROL 2 — the ARRAY family's own predicate terminal (the twin the
// issue says is already correct): `arr.FirstOrNull(lambda(x) => x > 5)`
// answers the matching element. It is green on the base and must stay green:
// #9261's sema half types the `*OrNull` predicate's parameter for EVERY
// receiver (the seeding table is shared), so a wrong fix that gave the
// array's parameter the wrong type (or that broke the array's `*OrNull`
// lowering) turns it red.
const char* kArrayControl =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    var arr := new Array<int>();\n"
    "    arr.Add(3);\n"
    "    arr.Add(9);\n"
    "    var v := arr.FirstOrNull(lambda(x) => x > 5);\n"
    "    if v = null then Console.WriteLine(\"none\"); else "
    "Console.WriteLine($\"first {v ?? -1}\"); end;\n"
    "  end Main;\n"
    "end Program;\n";

}  // namespace

BEZEL_TEST(codegen_dict_predicate_terminals_9261,
           the_first_with_a_predicate_returns_the_first_matching_entry_spec_5) {
    JitResult r = runAotChecked(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);          // no leak, no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The issue's own repro, verbatim: the matching entry is "b". On the base
    // the call lowered to NOTHING, the local was zero-filled and the
    // program SEGFAULTED on the null `Key`.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("first b\n"));
}

BEZEL_TEST(codegen_dict_predicate_terminals_9261,
           the_last_with_a_predicate_returns_the_last_matching_entry_spec_5) {
    JitResult r = runAotChecked(kLastRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("last b\n"));
}

BEZEL_TEST(codegen_dict_predicate_terminals_9261,
           the_single_with_a_predicate_returns_the_sole_matching_entry_spec_5) {
    JitResult r = runAotChecked(kSingleRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("single a\n"));
}

BEZEL_TEST(codegen_dict_predicate_terminals_9261,
           the_ornull_predicate_forms_return_the_matching_entry_spec_5) {
    JitResult r = runAotChecked(kOrNullRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // On the base every one of the three read as EMPTY ("first none",
    // "last none", "single none") — the predicate's body could not be typed
    // (the parameter stayed UNTYPED), so the closure folded to a constant
    // `false` and no entry ever matched.
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("first b 9\nlast b 9\nsingle b 9\n"));
}

BEZEL_TEST(codegen_dict_predicate_terminals_9261,
           the_singleornull_with_a_predicate_raises_on_a_second_match_spec_5) {
    JitResult r = runAotChecked(kSingleOrNullRaises);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // stdlib §5: "`SingleOrNull(pred)` still raises when more than one
    // matches". On the base the raise never happened (the predicate folded
    // to `false`, so there was no match at all) and this printed
    // "no raise none".
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("raised\n"));
}

BEZEL_TEST(codegen_dict_predicate_terminals_9261,
           the_predicate_form_over_int_keys_returns_the_matching_entry_spec_5) {
    JitResult r = runAot(kIntKeyRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("first 9 9\n"));
}

BEZEL_TEST(codegen_dict_predicate_terminals_9261,
           the_rebuilt_key_of_a_predicate_entry_is_released_spec_28_9) {
    JitResult r = runAotChecked(kLeakLoop);
    BEZEL_EXPECT_TRUE(r.ran);          // no leak (the bug), no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedPerIteration());
}

BEZEL_TEST(codegen_dict_predicate_terminals_9261,
           the_no_argument_ornull_tier_keeps_its_answer_spec_5) {
    JitResult r = runAotChecked(kNoArgOrNullControl);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("noarg none\n"));
}

BEZEL_TEST(codegen_dict_predicate_terminals_9261,
           the_array_family_predicate_terminal_keeps_its_answer_spec_5) {
    JitResult r = runAot(kArrayControl);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("first 9\n"));
}
