// Codegen — issue #6540: a `null` key into a `Dictionary<string?, V>` SEGFAULTS
// the generated program (exit 139, no diagnostic) on four separate members:
// `.Add(null, v)`, `.GetOrNull(null)`, `.ContainsKey(null)` and `null in d`.
//
// WHY THIS IS A FIX AND NOT A REJECTION. `Dictionary<string?, V>` is a legal
// instantiation and `null` is a legal `string?`, so the question "may `null` be
// a key?" is answered by the spec, and it answers YES:
//
//   * spec §3.6, *Derived type properties*, the `hashable` row, clause (c):
//     "a nullable type `T?` ... such a type's `Equals` and `GetHashCode` are
//     `T`'s, through §8.4's null-safe wrapper — which hashes `null` to ONE FIXED
//     IMPLEMENTATION-DEFINED VALUE". So `string?` is hashable and a `null` key
//     is storable, findable and removable. stdlib §4.8 declares
//     `Add(key: K, value: V)` / `ContainsKey(key: K)` / `GetOrNull(key: K)`
//     over `K`, with no non-null qualifier on `K`.
//   * spec §8.4: "`null = null` is `true`; `null = nonNull` and `nonNull =
//     null` are `false`" — which fixes the equality the table must implement.
//
// Rejecting the program would contradict §3.6's hashable row, so this is a
// behaviour-preserving encoder fix: no program that compiled before is
// rejected now, and the four that segfaulted now answer.
//
// MECHANISM (MEASURED, not inferred). `null` lowers to a null `bz_string*`, and
// every string-key encoder ran `loadStringData(k)` — `CreateStructGEP` off that
// null pointer, then a load — with no absent-key case. ASan on the pre-fix
// base `e0594ff4`:
//
//     ==2638096==ERROR: AddressSanitizer: SEGV on unknown address
//       0x000000000000 ... caused by a READ memory access
//         #0 ... in main
//
// with the program's own stdout marker proving WHICH call died: all four
// programs print their marker immediately before the offending call and
// nothing after it. Every row below asserts the MARKER LINES, not the exit
// code — 139 is the same number for a crash anywhere, so an exit-code-only
// assertion would pass on a crash in an unrelated place.
//
// THE FIX. `Emitter::dictStringKeyOperand` (LlvmBackend_impl.hpp, beside
// `loadStringData`/`loadStringLen`/`loadStringWidth`) substitutes a
// zero-initialised `bz_string` for a null key operand, whose (data, len, width)
// triple is exactly (null, 0, 0). Selecting a safe POINTER and loading from it
// is the load-bearing part: selecting the three already-loaded values would
// still fault, because the faulting load is emitted either way.
//
// Runtime side (bezel_runtime_collections.cpp):
//   * `fnv1a_cp(nullptr, …)` returns one FIXED value (§3.6's "one fixed
//     implementation-defined value"), distinct from the empty string's so the
//     two do not share every bucket;
//   * `bz_key_eq` decides `null = null` / `null = present` FIRST, so an absent
//     key (length 0) does not compare equal to `""` through the memcmp path;
//   * `bz_dict_try_get_str` no longer short-circuits on a null key — pre-fix
//     `ContainsKey(null)` after `Add(null, v)` could only ever answer false;
//   * the absent key is STORED with `key_w == 0` (a value no real `bz_string`
//     carries — §4.6 widths are 1/2/4) and its stored buffer is the ordinary
//     1-byte `bz_key_dup` allocation, so every free / copy / rehash / snapshot
//     path is unchanged. `bz_dict_keys_str` and `bz_dict_next_str` now preserve
//     the 0 instead of normalising it to 1, and `decodeDictStringKeyAt`
//     rebuilds a null `bz_string*` from it — so a `null` key read back out of a
//     `Dictionary<string?, V>` is `null`, not `""`.
//
// Oracles: `runAot` for every behavioural row (printed values, never "it
// compiled"), plus a `runAotChecked` row that requires the sanitizer to be
// CLEAN, which is what fails on the pre-fix SEGV.

#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Run a program and return its captured stdout, or a marker string when it did
// not run to completion. `runAot` fills `stdoutText` even for a program that
// dies mid-way, which is exactly what the marker rows need.
std::string out(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did-not-run>";
    return r.stdoutText;
}

// Wrap a body in the standard `module M; class C ... Main` shell.
std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

// runAotChecked + "the sanitizer found nothing". A pre-fix run is a SEGV (not
// a leak summary), so this is false before the fix and true after it.
bool sanitizerClean(const std::string& program) {
    auto r = runAotChecked(program);
    if (!r.ran) return false;   // toolchain / compile / link failure, not a pass
    return !r.diagnostics.hasErrors();
}

}  // namespace

// ── The issue's four repros, one per member, each with the stdout marker ────
// The marker is printed immediately before the offending call, so a row that
// sees "A:" but not "B:" localises the death to that one call — the exit code
// alone (139) cannot.

// 1. `.Add(null, 9)` — pre-fix SEGV after "A: before Add".
BEZEL_TEST(dict_null_key_6540, dict_null_key_add_no_crash_spec_8_4) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var e := new Dictionary<string?, int>(); "
            "Console.WriteLine(\"A: before Add\"); "
            "e.Add(null, 9); "
            "Console.WriteLine(\"B: after Add\"); "
            "Console.WriteLine($\"Count={e.Count.ToString()}\"); ")),
        "A: before Add\nB: after Add\nCount=1\n");
}

// 2. `.GetOrNull(null)` — pre-fix SEGV after "A: before GetOrNull(null)".
// The dictionary here holds `"a"`, so the CORRECT answer is `null` (the key is
// absent): the row pins that the call completes and answers, which is only
// possible once the null key is encoded rather than dereferenced.
BEZEL_TEST(dict_null_key_6540, dict_null_key_get_or_null_no_crash_spec_8_4) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var e := new Dictionary<string?, int>(); "
            "e.Add(\"a\", 9); "
            "Console.WriteLine(\"A: before GetOrNull(null)\"); "
            "var v := e.GetOrNull(null); "
            "Console.WriteLine($\"B: after = {(if v = null then "
            "\"null\" else v!.ToString())}\"); ")),
        "A: before GetOrNull(null)\nB: after = null\n");
}

// 3. `.ContainsKey(null)` — pre-fix SEGV after "A: before ContainsKey(null)".
BEZEL_TEST(dict_null_key_6540, dict_null_key_contains_key_no_crash_spec_8_4) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var e := new Dictionary<string?, int>(); "
            "e.Add(\"a\", 9); "
            "Console.WriteLine(\"A: before ContainsKey(null)\"); "
            "Console.WriteLine($\"B: after = "
            "{e.ContainsKey(null).ToString()}\"); ")),
        "A: before ContainsKey(null)\nB: after = False\n");
}

// 4. `null in e` — pre-fix SEGV after "A: before (null in e)".
BEZEL_TEST(dict_null_key_6540, dict_null_key_in_operator_no_crash_spec_8_4) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var e := new Dictionary<string?, int>(); "
            "e.Add(\"a\", 9); "
            "Console.WriteLine(\"A: before (null in e)\"); "
            "Console.WriteLine($\"B: after = {(null in e).ToString()}\"); ")),
        "A: before (null in e)\nB: after = False\n");
}

// ── §8.4: `null = null` is true — the round trip every member must agree on ──
// A row that only checked "did not crash" would be satisfied by an encoder that
// answered every question wrong; these assert the VALUES. `GetOrNull`,
// `ContainsKey`, `TryGetValue`, `in` and the indexer must all FIND the entry
// added under `null`, which is the whole content of §3.6's hashable row.
BEZEL_TEST(dict_null_key_6540,
           dict_null_key_round_trip_all_members_find_it_spec_8_4) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var e := new Dictionary<string?, int>(); "
            "e.Add(null, 9); "
            "Console.WriteLine($\"Count={e.Count.ToString()}\"); "
            "Console.WriteLine($\"ContainsKey={"
            "e.ContainsKey(null).ToString()}\"); "
            "var g := e.GetOrNull(null); "
            "Console.WriteLine($\"GetOrNull={(if g = null then \"null\" "
            "else g!.ToString())}\"); "
            "Console.WriteLine($\"in={(null in e).ToString()}\"); "
            "var ix := e[null]; "
            "Console.WriteLine($\"index={(if ix = null then \"null\" "
            "else ix!.ToString())}\"); "
            "Console.WriteLine($\"TryGetValue={"
            "e.TryGetValue(null, out var tv).ToString()}\"); ")),
        "Count=1\nContainsKey=True\nGetOrNull=9\nin=True\nindex=9\n"
        "TryGetValue=True\n");
}

// §8.4's other half: `null = nonNull` is FALSE, and the empty string is the
// shape that would collide with an absent key if the encoder did not mark it
// (both have length 0). Pinning `""` here is what makes the null ≠ "" rule
// load-bearing rather than incidental.
BEZEL_TEST(dict_null_key_6540,
           dict_null_key_is_not_the_empty_string_spec_8_4) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var e := new Dictionary<string?, int>(); "
            "e.Add(null, 9); "
            "e.Add(\"\", 5); "
            "Console.WriteLine($\"Count={e.Count.ToString()}\"); "
            "Console.WriteLine($\"ContainsNull={"
            "e.ContainsKey(null).ToString()}\"); "
            "Console.WriteLine($\"ContainsEmpty={"
            "e.ContainsKey(\"\").ToString()}\"); "
            "var a := e.GetOrNull(null); "
            "Console.WriteLine($\"GetNull={(if a = null then \"null\" "
            "else a!.ToString())}\"); "
            "var b := e.GetOrNull(\"\"); "
            "Console.WriteLine($\"GetEmpty={(if b = null then \"null\" "
            "else b!.ToString())}\"); "
            "Console.WriteLine($\"EmptyIn={((\"\" in e)).ToString()}\"); "
            "Console.WriteLine($\"NullIn={(null in e).ToString()}\"); ")),
        "Count=2\nContainsNull=True\nContainsEmpty=True\nGetNull=9\n"
        "GetEmpty=5\nEmptyIn=True\nNullIn=True\n");
}

// `Remove(null)` must unlink exactly the absent key — the dual of the Add row,
// and the path that also reaches `bz_dict_remove_str`'s `bz_key_eq`.
BEZEL_TEST(dict_null_key_6540, dict_null_key_remove_round_trip_spec_8_4) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var e := new Dictionary<string?, int>(); "
            "e.Add(null, 9); "
            "e.Add(\"a\", 1); "
            "Console.WriteLine($\"RemoveNull={e.Remove(null).ToString()}\"); "
            "Console.WriteLine($\"RemoveNullAgain={"
            "e.Remove(null).ToString()}\"); "
            "Console.WriteLine($\"Count={e.Count.ToString()}\"); "
            "Console.WriteLine($\"RemoveA={e.Remove(\"a\").ToString()}\"); "
            "Console.WriteLine($\"Count2={e.Count.ToString()}\"); ")),
        "RemoveNull=True\nRemoveNullAgain=False\nCount=1\nRemoveA=True\n"
        "Count2=0\n");
}

// The indexer-SET spelling `d[null] := v` reaches a DIFFERENT encoder arm
// (LlvmBackend_Assign.cpp's `lowerCollectionIndexerSet`) from the indexer get
// above, and the dictionary literal `Dictionary<string?, int> := [null: 9]`
// a fifth (LlvmBackend_Setup.cpp's literal materializer). Each gets its own
// row because a shared-name fix would not distinguish them.
BEZEL_TEST(dict_null_key_6540, dict_null_key_indexer_set_spec_8_4) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var e := new Dictionary<string?, int>(); "
            "e[null] := 9; "
            "Console.WriteLine($\"Count={e.Count.ToString()}\"); "
            "var g := e.GetOrNull(null); "
            "Console.WriteLine($\"Get={(if g = null then \"null\" "
            "else g!.ToString())}\"); ")),
        "Count=1\nGet=9\n");
}

BEZEL_TEST(dict_null_key_6540, dict_null_key_dictionary_literal_spec_8_4) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var e: Dictionary<string?, int> := [null: 9, \"a\": 1]; "
            "Console.WriteLine($\"Count={e.Count.ToString()}\"); "
            "var g := e.GetOrNull(null); "
            "Console.WriteLine($\"Get={(if g = null then \"null\" "
            "else g!.ToString())}\"); "
            "var h := e.GetOrNull(\"a\"); "
            "Console.WriteLine($\"GetA={(if h = null then \"null\" "
            "else h!.ToString())}\"); ")),
        "Count=2\nGet=9\nGetA=1\n");
}

// Read-back: a `null` key must come back out of `Keys` as `null`, not as `""`.
// This is the row that pins `bz_dict_keys_str`'s width-0 marker surviving the
// snapshot and `decodeDictStringKeyAt` turning it back into a null binding.
BEZEL_TEST(dict_null_key_6540, dict_null_key_read_back_as_null_spec_8_4) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var e := new Dictionary<string?, string>(); "
            "e.Add(null, \"N\"); "
            "e.Add(\"a\", \"A\"); "
            "for var k in e.Keys do "
            "  Console.WriteLine($\"key={(if k = null then \"<null>\" "
            "else k!)}\"); "
            "end for; ")),
        "key=<null>\nkey=a\n");
}

// ── Controls: the already-correct shapes must be untouched ──────────────────

// A NON-nullable `Dictionary<string, V>` is byte-identical after the fix (the
// `key.absent` select folds away on a key the type system already proved
// present). Without this control a fix that broke ordinary string keys would
// still pass every row above.
BEZEL_TEST(dict_null_key_6540, dict_non_nullable_string_key_control_spec_8_4) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var e := new Dictionary<string, int>(); "
            "e.Add(\"a\", 1); "
            "e.Add(\"b\", 2); "
            "Console.WriteLine($\"Count={e.Count.ToString()}\"); "
            "Console.WriteLine($\"HasA={e.ContainsKey(\"a\").ToString()}\"); "
            "Console.WriteLine($\"HasZ={e.ContainsKey(\"z\").ToString()}\"); "
            "var g := e.GetOrNull(\"b\"); "
            "Console.WriteLine($\"GetB={(if g = null then \"null\" "
            "else g!.ToString())}\"); "
            "for var k in e.Keys do "
            "  Console.WriteLine($\"key={k!}\"); "
            "end for; ")),
        "Count=2\nHasA=True\nHasZ=False\nGetB=2\nkey=a\nkey=b\n");
}

// The empty string as an ordinary key on a non-nullable dictionary: the shape
// the absent-key marker must NOT be confused with. (On the nullable dictionary
// the same shape is pinned by dict_null_key_is_not_the_empty_string above.)
BEZEL_TEST(dict_null_key_6540, dict_empty_string_key_control_spec_8_4) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var e := new Dictionary<string, int>(); "
            "e.Add(\"\", 5); "
            "Console.WriteLine($\"Has={e.ContainsKey(\"\").ToString()}\"); "
            "var g := e.GetOrNull(\"\"); "
            "Console.WriteLine($\"Get={(if g = null then \"null\" "
            "else g!.ToString())}\"); ")),
        "Has=True\nGet=5\n");
}

// A §4.6 width>1 key, so a fix that keyed the absent marker on "length 0"
// alone would be caught: this key has length 1 and width 2 and must be
// unaffected.
BEZEL_TEST(dict_null_key_6540, dict_wide_string_key_control_spec_4_6) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var e := new Dictionary<string, int>(); "
            "e.Add(\"\\u65E5\", 7); "
            "Console.WriteLine($\"Has={e.ContainsKey(\"\\u65E5\")"
            ".ToString()}\"); "
            "var g := e.GetOrNull(\"\\u65E5\"); "
            "Console.WriteLine($\"Get={(if g = null then \"null\" "
            "else g!.ToString())}\"); ")),
        "Has=True\nGet=7\n");
}

// ── The sanitizer row: the pre-fix failure is a SEGV, so this is the row
//    that distinguishes "the crash is gone" from "the values happen to match".
BEZEL_TEST(dict_null_key_6540, dict_null_key_asan_clean_spec_8_4) {
    BEZEL_EXPECT_TRUE(sanitizerClean(prog(
        "var e := new Dictionary<string?, int>(); "
        "e.Add(null, 9); "
        "e.Add(\"a\", 1); "
        "var a := e.GetOrNull(null); "
        "var b := e.GetOrNull(\"a\"); "
        "var c := e.ContainsKey(null); "
        "var d := null in e; "
        "var f := e[\"a\"]; "
        "var g := e.Remove(null); "
        "Console.WriteLine($\"{a!.ToString()}{b!.ToString()}"
        "{c.ToString()}{d.ToString()}{f.ToString()}{g.ToString()}\"); ")));
}

// Loop-amplified: one null key per iteration, so a per-call leak in the
// substituted-operand path would show as a leak SUMMARY rather than being lost
// in the noise of a single call. (Pre-fix this program SEGVs, so
// `sanitizerClean` is false there — which is what makes the row fail without
// the fix.)
BEZEL_TEST(dict_null_key_6540, dict_null_key_asan_clean_loop_spec_8_4) {
    BEZEL_EXPECT_TRUE(sanitizerClean(prog(
        "var e := new Dictionary<string?, int>(); "
        "var i := 0; "
        "while i < 200 do "
        "  e.Add(null, i); "
        "  var v := e.GetOrNull(null); "
        "  var c := e.ContainsKey(null); "
        "  var m := null in e; "
        "  if not c then return 2; end; "
        "  if not m then return 3; end; "
        "  if v = null then return 4; end; "
        "  i := i + 1; "
        "end while; "
        "Console.WriteLine($\"{e.Count.ToString()}\"); ")));
}