// #6557 — `Add` into a user-built `Array<DictionaryEntry<string, V>>` stored
// the entry's Key BORROWED, so the Key died with the for-in entry snapshot it
// was read from and every later `es[i].Key` read a freed `bz_string`.
//
// THE RULE (spec §28.9 "Runtime-managed types", §28.3.1 K1.6, §28.11; v0.357).
// `string` is runtime-managed (§28.9), and K1.6 gives a value of runtime-managed
// type "no classification and no obligation": the program never disposes a
// string and never has to keep its source alive — a stored copy simply stays
// valid. §28.11's value row makes `Array<DictionaryEntry<K, V>>.Add` a plain
// value copy of the `{Key, Value}` entry. Nothing in either rule lets the
// stored Key's validity depend on how long the DICTIONARY ITERATION that
// produced the entry runs.
//
// THE DEFECT. The implementation realizes "runtime-managed" by refcount, and a
// `for var e in d` loop over a string-keyed dictionary yields entries whose Key
// is REBUILT for the iteration and released when it ends. `es.Add(e)` was a
// byte copy that took no reference, so after the loop `es` held only pointers
// to released Keys:
//
//     for var e in d do es.Add(e); end for;
//     Console.WriteLine($"{es[0].Key} {es.Count}");   // heap-use-after-free
//
// MEASURED on the base (`0b30832a7`, `bezelc --sanitize=address`):
// `AddressSanitizer: heap-use-after-free`, READ of size 8, freed by `main`; the
// unchecked build printed whatever had reused the block.
//
// THE FIX. `new Array<DictionaryEntry<string, V>>(...)` stamps the array at
// construction (`bz_array_own_str_dict_keys`, while it is still empty) as a
// CO-OWNER of the Key of every slot it holds — the #6550 discipline the
// `ToArray()` snapshot already follows. Every runtime mutator keeps that true
// (#6560): `Add` / `Insert` take, the indexer store takes-then-releases,
// `RemoveAt` / `Clear` / the frees give back; and the stamp now SURVIVES
// `Clear`, so a re-filled array keeps owning what it stores.
// A span over the array is a second way to store a whole slot (`Fill`,
// `CopyTo`, `CopyFrom`); those stores take the same take-then-release path.
//
// ORACLE. Every row runs under `runAotChecked` — ASan plus strict LeakSanitizer
// — and asserts the exact stdout, that the program RAN TO COMPLETION and that
// nothing leaked or was over-released. The use-after-free is the crash half
// (`r.ran`); a fix that retained without ever releasing would pass that half
// and fail the leak half. Every row but the literal repro loops 100 times: one
// escaped Key is small enough to hide.
//
// NOT COVERED HERE, and deliberately not asserted: a `DictionaryEntry` LOCAL
// copied out of the array (`var t := es[0]`) is still a borrow of the slot's
// Key, so a swap through such a local on an array that is the Key's only owner
// reads a released Key — measured the same on a `ToArray()` snapshot, i.e. it
// predates this fix and is tracked separately.

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The harness SKIPS (loudly, counted) when clang is absent; a sanitizer finding
// is NOT a skip — "sanitizer fired" means it ran and found something.
bool skipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("SKIP") == std::string::npos) continue;
        if (d.message.find("sanitizer fired") != std::string::npos) return false;
        return true;
    }
    return false;
}

// Crash oracle AND leak oracle in one row: the program must run to completion
// under ASan, print exactly `expected`, exit 0, and report neither a leak nor
// an over-release.
void expectCleanRun(const std::string& program, const char* expected) {
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(expected));
    for (const auto& d : r.diagnostics.diagnostics()) {
        BEZEL_EXPECT_TRUE(d.message.find("over-release") == std::string::npos);
        BEZEL_EXPECT_TRUE(d.message.find("byte(s) leaked") == std::string::npos);
        BEZEL_EXPECT_TRUE(d.message.find("use-after-free") == std::string::npos);
    }
}

// One iteration of the mutation rows: a fresh three-entry dictionary whose Keys
// are distinct heap strings per iteration (`a{i}`, `bb{i}`, `ccc{i}` — lengths
// 2/3/4 for i < 10 and 3/4/5 after), an array filled from it by `Add` inside
// the for-in, then `ops`, then a read of every Key the array still holds.
std::string mutationProgram(const std::string& ops) {
    return
        "module M; class C public static method Main(): int "
        "  var n := 0; var i := 0; "
        "  while i < 100 do "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add($\"a{i}\", 1); d.Add($\"bb{i}\", 2); d.Add($\"ccc{i}\", 3); "
        "    var es := new Array<DictionaryEntry<string, int>>(4); "
        "    for var e in d do es.Add(e); end for; "
        "    " + ops + " "
        "    for var x in es do n := n + x.Key.Length + x.Value; end for; "
        "    n := n + es.Count; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C; ";
}

}  // namespace

// ---------------------------------------------------------------------------
// THE ISSUE'S OWN REPRO, verbatim.

BEZEL_TEST(array_dict_entry_add_key_coown_6557,
           issue_repro_add_in_for_in_then_read_key_spec_28_9) {
    expectCleanRun(
        "module M; class C public static method Main(): int\n"
        "var d := new Dictionary<string, string>(); d.Add(\"a\", \"1\"); d.Add(\"b\", \"2\");\n"
        "var es := new Array<DictionaryEntry<string, string>>();\n"
        "for var e in d do es.Add(e); end for;\n"
        "Console.WriteLine($\"{es[0].Key} {es.Count}\");\n"
        "return 0; end Main; end C;\n",
        "a 2\n");
}

// The ergonomics study's spelling of the same defect (issue comment, B5): an
// indexer-built dictionary, `end;` terminators, and every entry read back in
// array order. Insertion order is b, a, z (stdlib §4.8).
BEZEL_TEST(array_dict_entry_add_key_coown_6557,
           study_repro_every_entry_read_back_spec_28_9) {
    expectCleanRun(
        "module P;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var d := new Dictionary<string, int>();\n"
        "    d[\"b\"] := 1; d[\"a\"] := 4; d[\"z\"] := 2;\n"
        "    var es := new Array<DictionaryEntry<string, int>>();\n"
        "    for var e in d do es.Add(e); end;\n"
        "    for var e in es do Console.Write($\"{e.Key}{e.Value} \"); end;\n"
        "    Console.WriteLine();\n"
        "    Console.WriteLine(es.Count);\n"
        "  end Main;\n"
        "end Program;\n",
        "b1 a4 z2 \n3\n");
}

// The repro shape, 100 times, with heap Keys that differ per iteration and the
// `Add` behind a call boundary (the array is a parameter there, so the take
// cannot be a property of the construction site's static scope). Expected:
// (len("k{i}") + len("key{i}") + 2) summed = 10*8 + 90*10.
BEZEL_TEST(array_dict_entry_add_key_coown_6557,
           add_through_a_parameter_100_iterations_spec_28_9) {
    expectCleanRun(
        "module M; class C "
        "  public static method Fill(d: Dictionary<string, int>, "
        "                            es: Array<DictionaryEntry<string, int>>) "
        "    for var e in d do es.Add(e); end for; "
        "  end Fill; "
        "  public static method Main(): int "
        "    var n := 0; var i := 0; "
        "    while i < 100 do "
        "      var d := new Dictionary<string, int>(); "
        "      d.Add($\"k{i}\", 1); d.Add($\"key{i}\", 2); "
        "      var es := new Array<DictionaryEntry<string, int>>(); "
        "      C.Fill(d, es); "
        "      n := n + es[0].Key.Length + es[1].Key.Length + es.Count; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ",
        "980\n");
}

// The array OUTLIVES the dictionary it was filled from: the dictionary is a
// local of `Collect` and is gone before the caller reads a Key. Expected:
// (len("k{i}") + len("kk{i}")) summed = 10*5 + 90*7.
BEZEL_TEST(array_dict_entry_add_key_coown_6557,
           returned_array_outlives_its_dictionary_spec_28_9) {
    expectCleanRun(
        "module M; class C "
        "  public static method Collect(n: int): "
        "      owned Array<DictionaryEntry<string, string>> "
        "    var d := new Dictionary<string, string>(); "
        "    d.Add($\"k{n}\", \"v\"); d.Add($\"kk{n}\", \"w\"); "
        "    var es := new Array<DictionaryEntry<string, string>>(); "
        "    for var e in d do es.Add(e); end for; "
        "    return es; "
        "  end Collect; "
        "  public static method Main(): int "
        "    var n := 0; var i := 0; "
        "    while i < 100 do "
        "      var es := C.Collect(i); "
        "      n := n + es[0].Key.Length + es[1].Key.Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ",
        "680\n");
}

// ---------------------------------------------------------------------------
// EVERY MUTATOR keeps the array the owner of what it holds. Per iteration the
// three entries contribute key lengths 9 (i < 10) or 12, values 6, count 3.

// `Clear` then re-fill: the stamp must survive the `Clear`, or the second fill
// stores its Keys borrowed and the read below is the same use-after-free one
// `Clear` later. Expected 10*18 + 90*21.
BEZEL_TEST(array_dict_entry_add_key_coown_6557,
           clear_then_refill_keeps_owning_spec_28_11) {
    expectCleanRun(
        mutationProgram("es.Clear(); for var e in d do es.Add(e); end for;"),
        "2070\n");
}

// `Insert` takes like `Add`. Six entries: 10*36 + 90*42.
BEZEL_TEST(array_dict_entry_add_key_coown_6557,
           insert_takes_the_key_spec_28_11) {
    expectCleanRun(
        mutationProgram("for var e in d do es.Insert(0, e); end for;"),
        "4140\n");
}

// `RemoveAt` gives back exactly the removed slot's Key and leaves the
// survivors owned. Removes `bb{i}`: 10*12 + 90*14.
BEZEL_TEST(array_dict_entry_add_key_coown_6557,
           remove_at_releases_only_the_removed_key_spec_28_11) {
    expectCleanRun(mutationProgram("es.RemoveAt(1);"), "1380\n");
}

// An indexer store takes the incoming Key and releases the displaced one; the
// incoming Key is again a for-in entry's, dead after the loop. Slot 0 ends as
// `ccc{i}`: 10*22 + 90*25.
BEZEL_TEST(array_dict_entry_add_key_coown_6557,
           indexer_store_takes_incoming_releases_displaced_spec_28_11) {
    expectCleanRun(
        mutationProgram("for var e in d do es[0] := e; end for;"),
        "2470\n");
}

// Self-assignment is the identity (#6560's take-then-release ordering), on an
// array whose Keys have no other owner left. Expected as the plain fill.
BEZEL_TEST(array_dict_entry_add_key_coown_6557,
           indexer_self_store_is_the_identity_spec_28_11) {
    expectCleanRun(mutationProgram("es[1] := es[1];"), "2070\n");
}

// `new Array<...>(size, fill)`: the stamp is applied AFTER the fill, so each
// pre-populated slot holds its own reference on the fill entry's Key, and an
// element copied out of it into `es` is taken again there. Six entries.
BEZEL_TEST(array_dict_entry_add_key_coown_6557,
           size_fill_constructor_owns_every_filled_slot_spec_28_11) {
    expectCleanRun(
        mutationProgram(
            "for var e in d do "
            "  var filled := new Array<DictionaryEntry<string, int>>(3, e); "
            "  es.Add(filled[2]); "
            "end for;"),
        "4140\n");
}

// The zero-length `(size, fill)` array has no backing store when it is
// stamped; it must still take on its first `Add`.
BEZEL_TEST(array_dict_entry_add_key_coown_6557,
           zero_length_fill_constructor_still_takes_on_add_spec_28_11) {
    expectCleanRun(
        mutationProgram(
            "for var e in d do "
            "  var z := new Array<DictionaryEntry<string, int>>(0, e); "
            "  z.Add(e); "
            "  es.Add(z[0]); "
            "end for;"),
        "4140\n");
}

// ---------------------------------------------------------------------------
// SPAN WRITES. An `ArraySpan<T>` is a view over the array (stdlib §4.15), so a
// span write that replaces a whole slot is an indexer store on the backing
// array and must keep its Key count exact. Before these stored through
// `bz_array_set`, each of the three rows below aborted with `refcount
// over-release (string)`: the array released, at free, a Key the raw span store
// had never taken.

// `Fill` with a for-in entry: every slot ends as `ccc{i}`. 10*24 + 90*27.
BEZEL_TEST(array_dict_entry_add_key_coown_6557,
           span_fill_takes_per_slot_spec_4_15) {
    expectCleanRun(
        mutationProgram("for var e in d do es.AsSpan().Fill(e); end for;"),
        "2670\n");
}

// `CopyTo` from a second user-built array (reversed) into the first: same
// three entries in the other order, so the plain-fill total.
BEZEL_TEST(array_dict_entry_add_key_coown_6557,
           span_copy_to_takes_incoming_releases_displaced_spec_4_15) {
    expectCleanRun(
        mutationProgram(
            "var other := new Array<DictionaryEntry<string, int>>(); "
            "for var e in d do other.Add(e); end for; "
            "other.AsSpan().Reverse(); "
            "other.AsSpan().CopyTo(es.AsSpan());"),
        "2070\n");
}

// `CopyFrom` a `ToArray()` snapshot's read-only span: slots 0..1 become
// `bb{i}`, `ccc{i}`. 10*22 + 90*25.
BEZEL_TEST(array_dict_entry_add_key_coown_6557,
           span_copy_from_snapshot_takes_incoming_spec_4_15) {
    expectCleanRun(
        mutationProgram(
            "var other := d.ToArray(); "
            "es.AsSpan(0, 2).CopyFrom(other.AsReadOnlySpan(1, 2));"),
        "2470\n");
}

// ---------------------------------------------------------------------------
// THE GATE'S NEGATIVE SPACE: an INT-keyed entry array. Stdlib §4.8 stores int
// keys inline, so field 0 of the entry is not a string and a stamp keyed on
// "the element is a DictionaryEntry" would retain and release an integer.
// Expected: sum(i) + len("v{i}") summed + 2 per iteration = 4950 + 290 + 200.
BEZEL_TEST(array_dict_entry_add_key_coown_6557,
           control_int_keyed_entry_array_is_untouched_spec_4_8) {
    expectCleanRun(
        "module M; class C public static method Main(): int "
        "  var n := 0; var i := 0; "
        "  while i < 100 do "
        "    var d := new Dictionary<int, string>(); "
        "    d.Add(i, $\"v{i}\"); d.Add(i + 1000, \"w\"); "
        "    var es := new Array<DictionaryEntry<int, string>>(); "
        "    for var e in d do es.Add(e); end for; "
        "    n := n + es[0].Key + es[0].Value.Length + es.Count; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C; ",
        "5440\n");
}
