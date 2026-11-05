// Spec-derived regression test for issue #8169 — the ENTRY-SHAPED
// `ICollection<DictionaryEntry<K, V>>` forms of `Dictionary<K, V>`
// (`Add(entry)` / `Contains(entry)` / `Remove(entry)`) are not implemented.
//
// THE DECLARATION (stdlib §4.8 `Dictionary<K, V>`, the §4.8 code block,
// stdlib v0.355):
//
//     public class Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>
//       public method Add(key: K, value: V);
//       public method ContainsKey(key: K): bool;
//       public method Contains(key: K): bool;   // alias for ContainsKey
//       public method Remove(key: K): bool;
//       …
//     end Dictionary;
//
//     public record DictionaryEntry<K, V>(Key: K, Value: V);
//
// and stdlib §4.1's `ICollection<T>` (the interface `Dictionary<K, V>`
// conforms to) declares `Add(item: T)` / `Contains(item: T)` /
// `Remove(item: T)` — with `T` = `DictionaryEntry<K, V>` here. stdlib §1's
// elision rule makes those interface-required members part of the concrete
// class's own public contract, so the entry-shaped forms are reachable on a
// `Dictionary<K, V>` receiver.
//
// THE BEHAVIOUR (stdlib §4.8, v0.355, verbatim):
//
//   "The entry-shaped `ICollection<DictionaryEntry<K, V>>` forms (reachable per
//    §1's elision rule) behave as follows: `Add(entry)` is
//    `Add(entry.Key, entry.Value)` — `ArgumentException` on a present key;
//    `Contains(entry)` is `true` iff the key is present **and** the stored
//    value equals `entry.Value` under the type's standard equality;
//    `Remove(entry)` removes only when both match, else returns `false` with
//    no change."
//
// THE DEFECT (issue #8169, measured on the pre-fix build):
//
//     var src := new Dictionary<string, int>();
//     src.Add("a", 1);
//     var d := new Dictionary<string, int>();
//     for var e in src do
//       d.Add(e);
//       Console.WriteLine(d.Contains(e));
//     end for;
//
// was REJECTED with
//
//     `Add` on built-in type `Dictionary` produced no value, and no dispatch
//     arm emitted any code for this call. … (#2457)
//
// — the #2457 codegen backstop, because the dictionary's keyed `Add` arm is
// arity-2 (`Add(key, value)`) and no entry-shaped arm existed. MEASURED on
// the same pre-fix build, the two SIBLING entry forms were worse than a
// refusal: `d.Remove(e)` and `d.Contains(e)` were lowered as the KEYED
// forms with the whole `{K, V}` entry struct used as the key, and the
// generated IR failed verification ("Invalid operand types for ICmp
// instruction … `icmp eq { ptr, i32 }`") — a malformed-IR compile error
// rather than a diagnostic.
//
// THE FIX. `src/codegen/LlvmBackend_Calls.cpp`'s dictionary member dispatch
// grows the three entry-shaped arms. `Add(entry)` lowers to the SAME insert
// the keyed form performs (duplicate-key `ArgumentException` included);
// `Contains(entry)` and `Remove(entry)` are one `bz_dict_try_get_*` probe
// (which answers "present" and yields the stored value) followed by the
// type's standard equality — the authority the collection equality search
// uses (`emitRemoveElementEquality`: §8.3 record structural equality, §8.4
// class `Equals`/reference identity, §4.6 string content, §9.2 value
// equality).
//
// SCOPE. Only `Dictionary<K, V>` carries the entry forms. stdlib §4.9:
// "`OwningDictionary<K, V>` … The interface conformance is `IDisposable` only
// — not `IOwningCollection<DictionaryEntry<K, V>>`", and stdlib §4.17:
// `SortedDictionary<K, V>: IReadOnlyCollection<DictionaryEntry<K, V>>` — a
// READ-ONLY collection, so no `Add`/`Contains`/`Remove` entry form (its
// `Contains(key: K)` is the keyed alias). The two rows below pin that
// ruling: the entry form is REJECTED on those two heads.
//
// ⚠ THE ORACLE TRAP. `runAot` fills `diagnostics` only on a REJECT, so
// "this program draws no diagnostic X" is VACUOUS on a program that compiles
// (scripts/check_vacuous_tests.py). Every row below asserts on the program's
// OBSERVED OUTPUT (stdout / exit code), and every leak row goes through
// `runAotChecked` (which hardcodes `detect_leaks = true`); `runAotCheckedLenient`
// passes `detect_leaks = 0`, so a leak assertion written against it would be
// VACUOUS. A leak row is paired with a crash-oracle row that asserts the
// program RAN TO COMPLETION — a use-after-free ABORTS before LeakSanitizer
// prints its summary, so a leak row alone also reads as "clean" for a crash.

#include <initializer_list>
#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::checkedToolchainSkipped;
using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The issue's repro, VERBATIM (issue #8169's "THE DEFECT" block).
std::string reproProgram() {
    return
        "module M; import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var src := new Dictionary<string, int>();\n"
        "    src.Add(\"a\", 1);\n"
        "    var d := new Dictionary<string, int>();\n"
        "    for var e in src do\n"
        "      d.Add(e);\n"
        "      Console.WriteLine(d.Contains(e));\n"
        "    end for;\n"
        "    Console.WriteLine(d.Count);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// §4.8: "`Add(entry)` is `Add(entry.Key, entry.Value)` — `ArgumentException`
// on a present key". The duplicate key is the SAME key the entry carries, so
// the catch arm names the exception TYPE (an exit code alone would not
// distinguish `ArgumentException` from any other fault).
std::string duplicateKeyProgram() {
    return
        "module M; import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var src := new Dictionary<string, int>();\n"
        "    src.Add(\"a\", 1);\n"
        "    var d := new Dictionary<string, int>();\n"
        "    d.Add(\"a\", 5);\n"
        "    for var e in src do\n"
        "      try\n"
        "        d.Add(e);\n"
        "        Console.WriteLine(\"no-throw\");\n"
        "      catch\n"
        "        ArgumentException => Console.WriteLine(\"ArgumentException\")\n"
        "      end try;\n"
        "    end for;\n"
        "    Console.WriteLine(d.Count);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// 100 iterations with RUNTIME-PRODUCED string keys and values (an
// interpolated string is a fresh allocation; a literal is an immortal
// sentinel, so a literal could not tell a balanced reference count from an
// unbalanced one).
std::string hundredStringProgram() {
    return
        "module M; import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var src := new Dictionary<string, string>();\n"
        "    var i := 0;\n"
        "    while i < 100 do\n"
        "      src.Add($\"k{i}\", $\"v{i}\");\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    var d := new Dictionary<string, string>();\n"
        "    for var e in src do\n"
        "      d.Add(e);\n"
        "    end for;\n"
        "    Console.WriteLine(d.Count);\n"
        "    Console.WriteLine(d[\"k42\"]);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// The OWNING variant: both dictionaries are `using var` bindings, so each
// is DISPOSED at its scope end and every value it co-owns is released
// exactly once (§28.3 / §28.9). Under the sanitizer this row is the one
// that can see a missing retain, a double release, or a use-after-free.
std::string owningVariantProgram() {
    return
        "module M; import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var src := new Dictionary<string, string>();\n"
        "    var i := 0;\n"
        "    while i < 100 do\n"
        "      src.Add($\"k{i}\", $\"v{i}\");\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    using var d := new Dictionary<string, string>();\n"
        "    var hits := 0;\n"
        "    for var e in src do\n"
        "      d.Add(e);\n"
        "      if d.Contains(e) then hits := hits + 1; end;\n"
        "    end for;\n"
        "    Console.WriteLine(hits);\n"
        "    Console.WriteLine(d.Count);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// §4.8: "`Contains(entry)` is `true` iff the key is present **and** the
// stored value equals `entry.Value`"; "`Remove(entry)` removes only when
// both match, else returns `false` with no change". The stored value
// DIFFERS from the entry's, so both members answer `false` and the
// dictionary is unchanged.
std::string valueMismatchProgram() {
    return
        "module M; import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var src := new Dictionary<string, int>();\n"
        "    src.Add(\"a\", 1);\n"
        "    var d := new Dictionary<string, int>();\n"
        "    d.Add(\"a\", 99);\n"
        "    for var e in src do\n"
        "      Console.WriteLine(d.Contains(e));\n"
        "      Console.WriteLine(d.Remove(e));\n"
        "    end for;\n"
        "    Console.WriteLine(d.Count);\n"
        "    Console.WriteLine(d[\"a\"]);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// §4.8: "`Remove(entry)` removes only when both match" — the MATCHING case
// removes the entry (the value matches), and the dictionary is empty
// afterwards.
std::string valueMatchProgram() {
    return
        "module M; import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var src := new Dictionary<string, int>();\n"
        "    src.Add(\"a\", 1);\n"
        "    var d := new Dictionary<string, int>();\n"
        "    for var e in src do\n"
        "      d.Add(e);\n"
        "      Console.WriteLine(d.Remove(e));\n"
        "    end for;\n"
        "    Console.WriteLine(d.Count);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// The KEYED forms, unchanged: the entry arm must not swallow them.
std::string keyedFormsProgram() {
    return
        "module M; import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var d := new Dictionary<string, int>();\n"
        "    d.Add(\"a\", 1);\n"
        "    Console.WriteLine(d.Contains(\"a\"));\n"
        "    Console.WriteLine(d.Remove(\"a\"));\n"
        "    Console.WriteLine(d.Count);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// A NON-STRING key: the entry form's key half is the generic-key path
// (`bz_dict_*_gen`), not the string fast path.
std::string genericKeyProgram() {
    return
        "module M; import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var src := new Dictionary<int, string>();\n"
        "    src.Add(7, \"seven\");\n"
        "    var d := new Dictionary<int, string>();\n"
        "    for var e in src do\n"
        "      d.Add(e);\n"
        "      Console.WriteLine(d.Contains(e));\n"
        "      Console.WriteLine(d[e.Key]);\n"
        "    end for;\n"
        "    Console.WriteLine(d.Count);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// §4.9: `OwningDictionary<K, V>` conforms to `IDisposable` ONLY — it has no
// `ICollection<DictionaryEntry<K, V>>` conformance, hence NO entry form.
std::string owningDictionaryEntryProgram() {
    return
        "module M; import System;\n"
        "public class Val\n"
        "  public N: int;\n"
        "  public constructor(n: int) N := n; end constructor;\n"
        "end Val;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var src := new Dictionary<string, int>();\n"
        "    src.Add(\"a\", 1);\n"
        "    var d := new OwningDictionary<string, Val>();\n"
        "    for var e in src do\n"
        "      d.Add(e);\n"
        "    end for;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// §4.17: `SortedDictionary<K, V>` conforms to `IReadOnlyCollection<...>` —
// a read-only collection, so it declares no `Add` at all.
std::string sortedDictionaryEntryProgram() {
    return
        "module M; import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var src := new Dictionary<string, int>();\n"
        "    src.Add(\"a\", 1);\n"
        "    var d := new SortedDictionary<string, int>();\n"
        "    for var e in src do\n"
        "      d.Add(e);\n"
        "    end for;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// An ERROR-severity diagnostic whose message carries `needle`.
bool hasError(const std::string& program, const char* needle) {
    JitResult r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// A LEAK-oracle row: strict LSan armed, and the program must run clean.
// `expected` is the program's EXACT stdout.
void expectNoLeakExact(const std::string& program, const std::string& expected) {
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_TRUE(d.message.find("byte(s) leaked") == std::string::npos);
}

// A CRASH-oracle row: the program must RUN TO COMPLETION, print `expected`,
// and exit 0. A leak row alone cannot prove the absence of a use-after-free
// or a double free (a UAF aborts before LSan prints its summary, so the leak
// row would read as clean).
void expectRunsCleanExact(const std::string& program,
                          const std::string& expected) {
    JitResult r = runAotCheckedLenient(program);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// ── THE REPRO ───────────────────────────────────────────────────────────────

BEZEL_TEST(dictionary_add_entry_8169,
           the_repro_adds_the_entry_and_reads_it_back_spec_4_8) {
    // The issue's repro, VERBATIM. On the pre-fix build it was REJECTED
    // ("`Add` on built-in type `Dictionary` produced no value, and no
    // dispatch arm emitted any code for this call"), so `ran` is the
    // assertion that is RED there and GREEN here.
    //
    // The expected output is the issue's own: "The program above prints
    // `True` then `1`."
    expectNoLeakExact(reproProgram(), "True\n1\n");
    expectRunsCleanExact(reproProgram(), "True\n1\n");
}

// ── THE DUPLICATE KEY (the §4.8-stated exception) ──────────────────────────

BEZEL_TEST(dictionary_add_entry_8169,
           a_duplicate_key_raises_argument_exception_spec_4_8) {
    // §4.8: "`Add(entry)` is `Add(entry.Key, entry.Value)` — ArgumentException
    // on a present key". The catch arm names the exception TYPE, so the row
    // distinguishes the §4.8-stated `ArgumentException` from any other fault
    // (an exit code alone would not).
    expectNoLeakExact(duplicateKeyProgram(), "ArgumentException\n1\n");
    expectRunsCleanExact(duplicateKeyProgram(), "ArgumentException\n1\n");
}

// ── THE 100x LOOP WITH STRING KEYS AND VALUES ───────────────────────────────

BEZEL_TEST(dictionary_add_entry_8169,
           a_hundred_string_keys_and_values_leak_nothing_spec_4_8) {
    // 100 iterations of the entry-shaped insert with RUNTIME-PRODUCED string
    // keys and values: the reference counts have to balance exactly (the
    // source table's own +1 per stored value, the entry's own reference, the
    // destination table's +1 on the borrowed value, and each table's
    // scope-end release), or this leaks.
    expectNoLeakExact(hundredStringProgram(), "100\nv42\n");
    expectRunsCleanExact(hundredStringProgram(), "100\nv42\n");
}

// ── THE OWNING VARIANT ──────────────────────────────────────────────────────

BEZEL_TEST(dictionary_add_entry_8169,
           the_owning_variant_leaks_nothing_spec_4_8_spec_28_3) {
    // Both dictionaries are `using var` bindings, so each is DISPOSED at its
    // scope end. The entry-shaped insert's retained VALUE (the destination
    // table takes its own +1 on the entry's borrowed value — §28.9's
    // "every owner holds its own reference") has to be released exactly once
    // by that disposal: a missing release leaks, a double release is a
    // use-after-free the crash oracle below would see.
    expectNoLeakExact(owningVariantProgram(), "100\n100\n");
    expectRunsCleanExact(owningVariantProgram(), "100\n100\n");
}

// ── THE "BOTH MATCH" RULE ───────────────────────────────────────────────────

BEZEL_TEST(dictionary_add_entry_8169,
           contains_and_remove_need_the_value_to_match_spec_4_8) {
    // §4.8: "`Contains(entry)` is `true` iff the key is present **and** the
    // stored value equals `entry.Value` under the type's standard equality";
    // "`Remove(entry)` removes only when both match, else returns `false`
    // with no change". The key IS present but the stored value DIFFERS, so
    // both answer `false` and the dictionary is unchanged (Count 1, the
    // stored value still 99).
    expectNoLeakExact(valueMismatchProgram(), "False\nFalse\n1\n99\n");
    expectRunsCleanExact(valueMismatchProgram(), "False\nFalse\n1\n99\n");
}

BEZEL_TEST(dictionary_add_entry_8169,
           a_matching_entry_is_removed_spec_4_8) {
    // The matching half of the same rule: the key is present AND the value
    // matches, so `Remove(entry)` removes the entry and answers `true`.
    expectNoLeakExact(valueMatchProgram(), "True\n0\n");
    expectRunsCleanExact(valueMatchProgram(), "True\n0\n");
}

// ── A NON-STRING KEY ────────────────────────────────────────────────────────

BEZEL_TEST(dictionary_add_entry_8169,
           the_entry_form_works_for_a_generic_key_spec_4_8) {
    // The entry's Key half is the generic-key path (`bz_dict_*_gen`), not the
    // string fast path — the row keeps the two apart.
    expectNoLeakExact(genericKeyProgram(), "True\nseven\n1\n");
    expectRunsCleanExact(genericKeyProgram(), "True\nseven\n1\n");
}

// ── THE KEYED FORMS ARE UNCHANGED (control) ─────────────────────────────────

BEZEL_TEST(dictionary_add_entry_8169,
           the_keyed_forms_still_work_spec_4_8) {
    // CONTROL: the entry arm must not swallow the keyed forms. This row is
    // GREEN on the pre-fix build as well — it is the positive control for
    // the rows above, not a fail-without row.
    expectNoLeakExact(keyedFormsProgram(), "True\nTrue\n0\n");
}

// ── THE SCOPE RULING: no entry form on the other two heads ─────────────────

BEZEL_TEST(dictionary_add_entry_8169,
           owning_dictionary_declares_no_entry_form_stdlib_4_9) {
    // §4.9: "`OwningDictionary<K, V>` … The interface conformance is
    // `IDisposable` only — not `IOwningCollection<DictionaryEntry<K, V>>`."
    // There is no `ICollection` conformance, hence no entry-shaped `Add`.
    // CONTROL: this refusal is the arity rule (§4.9 declares `Add` with
    // exactly two parameters) and is GREEN on the pre-fix build too.
    BEZEL_EXPECT_TRUE(hasError(owningDictionaryEntryProgram(),
                                "OwningDictionary.Add"));
    BEZEL_EXPECT_TRUE(hasError(owningDictionaryEntryProgram(),
                                "exactly 2 argument(s)"));
}

BEZEL_TEST(dictionary_add_entry_8169,
           sorted_dictionary_declares_no_entry_form_stdlib_4_17) {
    // §4.17: `SortedDictionary<K, V>: IReadOnlyCollection<DictionaryEntry<K,
    // V>>` — a READ-ONLY collection, so it declares no `Add` at all (its
    // `Contains(key: K)` is the keyed alias). CONTROL, as above.
    BEZEL_EXPECT_TRUE(hasError(sortedDictionaryEntryProgram(),
                                "SortedDictionary.Add"));
    BEZEL_EXPECT_TRUE(hasError(sortedDictionaryEntryProgram(),
                                "exactly 2 argument(s)"));
}
