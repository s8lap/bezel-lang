// fix #6551 (§5.1, v0.338; §23.4, v0.338; stdlib §4.8/§4.9/§4.17): the
// dictionary INDEXER key slot is an ordinary declared parameter position, so
// §5.1 governs it exactly as it governs the five sibling members.
//
// THE SPEC. stdlib §4.8 declares exactly ONE indexer on `Dictionary<K, V>` —
// `public property [key: K]: V` — and the `K` it writes there is the SAME
// written type it writes for `ContainsKey(key: K)`, `Contains(key: K)`,
// `Remove(key: K)`, `Add(key: K, value: V)`, `TryAdd(key: K, value: V)`,
// `GetOrNull(key: K)` and `TryGetValue(key: K, out value: V?)` (§4.9 and §4.17
// declare the same pair for `OwningDictionary<K, V>` / `SortedDictionary<K, V>`).
// §23.4 makes the parameter-TYPE LIST what admits an index ("Indexers may be
// overloaded by parameter type list"), and §5.1 is what decides whether an
// argument is admitted at a declared parameter type: its table is numeric
// widening only, and its closing words are "Constant narrowing applies at every
// position where this section's implicit conversions apply — there is no
// position carve-out … the list is illustrative, not closed". Its
// never-implicit bullet — "Between signed and unsigned types of the same or
// narrower width (e.g. `int → uint`) … This bullet governs non-constant
// expressions" — names no exception for an index argument. So there is no raw
// key slot and no escape hatch: one rule, six positions.
//
// WHAT THE PRE-FIX COMPILER DID. The `isDict` branch of the IndexExpr
// typecheck never compared the index's type to `genericArgs[0]` and typed every
// `d[<anything>]` as `V`. MEASURED on this lane's base, `var i := 7` against
// `Dictionary<ulong, string> := [7: "S"]`, one member isolated per program:
//
//   d[i]         ACCEPTED, no diagnostic      <- this issue
//   d[i] := v    ACCEPTED, no diagnostic      <- this issue
//   i in d       rejected, §5.1 wording naming the member   (#6527)
//   d.GetOrNull(i)  rejected, §5.1 wording naming the member (#6527)
//   d.ContainsKey(i) / d.Remove(i) / d.Add(i, v)
//                                     rejected, §5.1 wording naming the member
//
// The accepted answer is a SIDE EFFECT of a codegen fix, not a decision:
// #6541's `coerceDictKeyToKeyType` zero-extends an `int` key at the indexer,
// so `d[i]` happens to read the right entry today. That is memory-safety
// adjacent rather than cosmetic — §5.2 makes a signed/unsigned conversion a
// CHECKED `OverflowException` — and admitting it implicitly at one member of
// six is what would turn any future narrowing at a key slot into a silent wrap
// at the indexer and nowhere else.
//
// The gap is the missing §5.1 GATE, not the `int` spelling: `d[3.5]`, a `bool`
// and a `string` were accepted at the same position on the base too (§5.1's
// table has no row for any of them), and a fix matching `int` would have left
// every one of them reading a key the other five members refuse. Rows below.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"          // runAot
#include "test_harness.hpp"

using namespace bezel::test;

namespace {

std::string prog(const std::string& mainBody,
                 const std::string& decls = "") {
    return "module M;\n" + decls +
           "class C\n  public static method Main(): int\n" + mainBody +
           "  end Main;\nend C;\n";
}

// fix #3892: `r.ran == false` ALONE is satisfied by a crash, a signal, a
// timeout and an ASan report as well as by a genuine rejection. What
// discriminates is the WORDING — pre-fix, this message does not exist anywhere
// in the output under any outcome, and on this issue's own repro the pre-fix
// outcome was a successful compile at exit 0. Both halves are asserted.
bool rejectedWith(const JitResult& r, const char* msg) {
    return failedAtCompileTimeWithRealLocation(r) &&
           diagnosticMessageContains(r, msg);
}

// The §5.1 wording the indexer now reports. It is the SHARED needle every §5.1
// site reports (`checkAssignable`'s one message), so these rows pin the rule
// reaching this position rather than a bespoke string.
const char* kIdxNeedle =
    "the index argument to `Dictionary[key]` has a type that is not "
    "implicitly convertible to the target type (§5.1)";

// stdout of a program that must compile, run and exit 0. `ran == false` reads
// back as a marker no expectation equals, so a rejection FAILS an accept row
// rather than passing it.
std::string out(const std::string& mainBody,
                const std::string& decls = "") {
    auto r = runAot(prog(mainBody, decls));
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// ── The issue's own repro: the indexer GET ─────────────────────────────────

BEZEL_TEST(dict_indexer_key_6551,
           issue_repro_indexer_get_non_implicit_key_rejected_spec_5_1) {
    auto r = runAot(prog("    var d: Dictionary<ulong, string> := "
                         "[7: \"S\"];\n"
                         "    var i := 7;\n"
                         "    var s := d[i];\n"
                         "    Console.WriteLine(s);\n"
                         "    return 0;\n"));
    // Pre-fix: exit 0, no diagnostic, and the right answer — by #6541's
    // zero-extension, not by a check. Nothing on the pre-fix compiler
    // produces this message, so this row is unsatisfiable there.
    BEZEL_EXPECT_TRUE(rejectedWith(r, kIdxNeedle));
}

// ── The indexer SET — the second arm of the same IndexExpr node ─────────────

BEZEL_TEST(dict_indexer_key_6551,
           indexer_set_non_implicit_key_rejected_spec_5_1) {
    auto r = runAot(prog("    var d: Dictionary<ulong, string> := "
                         "[7: \"S\"];\n"
                         "    var i := 7;\n"
                         "    d[i] := \"X\";\n"
                         "    Console.WriteLine(d[7]);\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, kIdxNeedle));
}

// ── The gap is the §5.1 GATE, not the `int` spelling ───────────────────────
//
// §5.1's implicit-conversion table is numeric widening only; there is no row
// converting a `double`, a `bool` or a `string` to an `int` key. A fix matching
// the `int` shape would have left all three reading a key the five sibling
// members refuse. Each row asserts the SHARED §5.1 needle, not "did not
// compile" — on this issue's repro the pre-fix outcome was a clean exit 0.

BEZEL_TEST(dict_indexer_key_6551,
           double_key_at_an_int_key_slot_rejected_spec_5_1) {
    auto r = runAot(prog("    var d: Dictionary<int, string> := "
                         "[3: \"S\"];\n"
                         "    var x := 3.5;\n"
                         "    Console.WriteLine(d[x]);\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, kIdxNeedle));
}

BEZEL_TEST(dict_indexer_key_6551,
           bool_key_at_an_int_key_slot_rejected_spec_5_1) {
    auto r = runAot(prog("    var d: Dictionary<int, string> := "
                         "[3: \"S\"];\n"
                         "    var b := true;\n"
                         "    Console.WriteLine(d[b]);\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, kIdxNeedle));
}

BEZEL_TEST(dict_indexer_key_6551,
           string_key_at_an_int_key_slot_rejected_spec_5_1) {
    auto r = runAot(prog("    var d: Dictionary<int, string> := "
                         "[3: \"S\"];\n"
                         "    var s := \"x\";\n"
                         "    Console.WriteLine(d[s]);\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, kIdxNeedle));
}

// A `Range` where a `K` is wanted is the SHAPE half #6500 already rejects at
// this position (§23.4). It stays rejected, with #6500's own message and not
// the §5.1 one: this fix is gated on `!(rangeIdx && !keyIsRange)` precisely so
// the two do not double-report. Pre-fix this row was already green; it is here
// so a future change to the gate cannot turn it into a §5.1 message or, worse,
// into an acceptance.
BEZEL_TEST(dict_indexer_key_6551,
           range_index_keeps_the_6500_shape_diagnostic_spec_23_4) {
    auto r = runAot(prog("    var d: Dictionary<int, string> := "
                         "[3: \"S\"];\n"
                         "    var s := d[0..1];\n"
                         "    return s.Length;\n"));
    BEZEL_EXPECT_TRUE(
        rejectedWith(r, "range index `[a..b]` is not admitted on "
                        "`Dictionary<K, V>`"));
}

// ── §5.1's ADMITTED conversions — the direction this fix must not break ─────
//
// Every row below compiles, runs and prints, so each is satisfied only by a
// compiler that admits the conversion AND materialises it correctly. These are
// the rows an over-broad predicate — one that checked shape, spelling, or
// "narrower than int" instead of asking §5.1 — would break. Each names the §5.1
// row that admits it.
//
// The values are read back through the WIDENED slot, so a compiler that admits
// the conversion but drops or truncates it prints the wrong thing.

BEZEL_TEST(dict_indexer_key_6551,
           int_widens_to_long_key_and_reads_back_spec_5_1) {
    BEZEL_EXPECT_EQ(std::string("S\n"),
                    out("    var d: Dictionary<long, string> := "
                        "[7: \"S\"];\n"
                        "    var i := 7;\n"
                        "    Console.WriteLine(d[i]);\n"
                        "    return 0;\n"));
}

BEZEL_TEST(dict_indexer_key_6551,
           int_widens_to_double_key_and_reads_back_spec_5_1) {
    BEZEL_EXPECT_EQ(std::string("S\n"),
                    out("    var d: Dictionary<double, string> := "
                        "[7: \"S\"];\n"
                        "    var i := 7;\n"
                        "    Console.WriteLine(d[i]);\n"
                        "    return 0;\n"));
}

BEZEL_TEST(dict_indexer_key_6551,
           int_widens_to_float_key_and_reads_back_spec_5_1) {
    BEZEL_EXPECT_EQ(std::string("S\n"),
                    out("    var d: Dictionary<float, string> := "
                        "[7: \"S\"];\n"
                        "    var i := 7;\n"
                        "    Console.WriteLine(d[i]);\n"
                        "    return 0;\n"));
}

BEZEL_TEST(dict_indexer_key_6551,
           byte_widens_to_short_key_and_reads_back_spec_5_1) {
    BEZEL_EXPECT_EQ(std::string("S\n"),
                    out("    var d: Dictionary<short, string> := "
                        "[7: \"S\"];\n"
                        "    var b: byte := 7;\n"
                        "    Console.WriteLine(d[b]);\n"
                        "    return 0;\n"));
}

BEZEL_TEST(dict_indexer_key_6551,
           ushort_widens_to_int_key_and_reads_back_spec_5_1) {
    BEZEL_EXPECT_EQ(std::string("S\n"),
                    out("    var d: Dictionary<int, string> := "
                        "[7: \"S\"];\n"
                        "    var b: ushort := 7;\n"
                        "    Console.WriteLine(d[b]);\n"
                        "    return 0;\n"));
}

// §5.1's never-implicit bullet's own counterpart clause: "Widening from
// unsigned to a strictly larger signed type (e.g. `byte → short`, `ushort →
// int`) is permitted because no value is lost." `uint → ulong` is the same
// direction across signedness, and §5.1's table admits it.
BEZEL_TEST(dict_indexer_key_6551,
           uint_widens_to_ulong_key_and_reads_back_spec_5_1) {
    BEZEL_EXPECT_EQ(std::string("S\n"),
                    out("    var d: Dictionary<ulong, string> := "
                        "[7: \"S\"];\n"
                        "    var b: uint := 7;\n"
                        "    Console.WriteLine(d[b]);\n"
                        "    return 0;\n"));
}

BEZEL_TEST(dict_indexer_key_6551,
           sbyte_widens_to_short_key_and_reads_back_spec_5_1) {
    BEZEL_EXPECT_EQ(std::string("S\n"),
                    out("    var d: Dictionary<short, string> := "
                        "[7: \"S\"];\n"
                        "    var b: sbyte := 7;\n"
                        "    Console.WriteLine(d[b]);\n"
                        "    return 0;\n"));
}

// §5.1's table lists `char` as a SOURCE converting to `int`, `uint`, `long`,
// `ulong`, `float`, `double` and `decimal` — an all-signedness row, and the one
// key type here that is neither an integer primitive widening nor an identity.
// The stored key is `'7'`'s VALUE (55), not the digit, so this row is only
// green if the char is materialised at the widened slot and hashed as 55.
BEZEL_TEST(dict_indexer_key_6551,
           char_widens_to_ulong_key_and_reads_back_spec_5_1) {
    BEZEL_EXPECT_EQ(std::string("S\n"),
                    out("    var d: Dictionary<ulong, string> := "
                        "[55: \"S\"];\n"
                        "    var c := '7';\n"
                        "    Console.WriteLine(d[c]);\n"
                        "    return 0;\n"));
}

BEZEL_TEST(dict_indexer_key_6551,
           float_widens_to_double_key_and_reads_back_spec_5_1) {
    BEZEL_EXPECT_EQ(std::string("S\n"),
                    out("    var d: Dictionary<double, string> := "
                        "[7.5: \"S\"];\n"
                        "    var f := 7.5;\n"
                        "    Console.WriteLine(d[f]);\n"
                        "    return 0;\n"));
}

BEZEL_TEST(dict_indexer_key_6551,
           long_widens_to_double_key_and_reads_back_spec_5_1) {
    BEZEL_EXPECT_EQ(std::string("S\n"),
                    out("    var d: Dictionary<double, string> := "
                        "[7: \"S\"];\n"
                        "    var l: long := 7;\n"
                        "    Console.WriteLine(d[l]);\n"
                        "    return 0;\n"));
}

// ── §5.1's CONSTANT NARROWING — value-based, and it CROSSES signedness ─────
//
// "An expression of integer type that is a compile-time constant implicitly
// converts to any integer type whose range contains its value … The rule is
// value-based, not type-based … and the target any integer type whose range
// contains the value, across signedness". Both of these are §5.1's own named
// example (`Set(3)` at a `byte` parameter) at a dictionary indexer, and both
// CROSS the signedness the non-constant rows above reject. An index argument is
// an "argument at a determined parameter type" (§17.7 (vi)), so the clause
// applies here identically — `fromExpr` is passed to `checkAssignable` for
// exactly this reason.

BEZEL_TEST(dict_indexer_key_6551,
           constant_narrows_to_ulong_key_across_signedness_spec_5_1) {
    BEZEL_EXPECT_EQ(std::string("S\n"),
                    out("    var d: Dictionary<ulong, string> := "
                        "[7: \"S\"];\n"
                        "    Console.WriteLine(d[7]);\n"
                        "    return 0;\n"));
}

BEZEL_TEST(dict_indexer_key_6551,
           constant_narrows_to_byte_key_and_reads_back_spec_5_1) {
    BEZEL_EXPECT_EQ(std::string("S\n"),
                    out("    var d: Dictionary<byte, string> := "
                        "[5: \"S\"];\n"
                        "    Console.WriteLine(d[5]);\n"
                        "    return 0;\n"));
}

// ── The remaining §5.1 surfaces at this position ───────────────────────────
//
// None of these is an integer conversion at all, so §5.1's table has nothing to
// say about them and they are decided on their own terms — a predicate that
// answered "is this key type numeric?" rather than "does §5.1 admit this?" would
// reject all three.

BEZEL_TEST(dict_indexer_key_6551,
           identity_string_key_is_unchanged_spec_5_1) {
    BEZEL_EXPECT_EQ(std::string("S\n"),
                    out("    var d: Dictionary<string, string> := "
                        "[\"k\": \"S\"];\n"
                        "    Console.WriteLine(d[\"k\"]);\n"
                        "    return 0;\n"));
}

// A NULLABLE key slot legitimately admits `null` (stdlib §4.8's `K` is `string?`
// here; #6540 made it a key rather than a segfault). §5.1's nullable-lifting
// paragraph admits `null` to `null`.
BEZEL_TEST(dict_indexer_key_6551,
           null_at_a_nullable_key_slot_is_a_key_spec_5_1) {
    BEZEL_EXPECT_EQ(std::string("S\n"),
                    out("    var d: Dictionary<string?, string> := "
                        "[null: \"S\"];\n"
                        "    Console.WriteLine(d[null]);\n"
                        "    return 0;\n"));
}

// `Dictionary<Range<T>, V>` is a legal key type (stdlib §9.8's auto-hashable
// `Range<T>`; #6207's whole surface). The indexer's key slot admits a `Range`
// here because `K` IS a `Range` — the exemption is the key TYPE, not the index
// spelling. Three keys share a `Start` and differ in §16.3's `IsEndInclusive` /
// §16.4's `Step`, so the row is unsatisfiable by anything reading one word (the
// exact reading that made `Dictionary<int, V>` wrong). `7 + 80 + 900 = 987`.
BEZEL_TEST(dict_indexer_key_6551,
           range_typed_key_lookup_is_unchanged_spec_8_3) {
    BEZEL_EXPECT_EQ(
        std::string("987\n"),
        out("    using var d := new Dictionary<Range<int>, int>();\n"
            "    d[1..5] := 7;\n"
            "    d[1..<5] := 8;\n"
            "    d[1..5 step 2] := 9;\n"
            "    var a := d[1..5];\n"
            "    var b := d[1..<5];\n"
            "    var c := d[1..5 step 2];\n"
            "    Console.WriteLine((a + b * 10 + c * 100).ToString());\n"
            "    return 0;\n"));
}

// ── The sibling families of the SAME indexer declaration ───────────────────
//
// stdlib §4.9 and §4.17 declare the identical `public property [key: K]: V` on
// `OwningDictionary<K, V>` and `SortedDictionary<K, V>`, so the same §5.1 gate
// reaches them: `OwningDictionary` at the indexer GET, `SortedDictionary` at the
// indexer. Measured before the fix, both were accepted at the same non-implicit
// `int` key as `Dictionary`.

BEZEL_TEST(dict_indexer_key_6551,
           sorted_dictionary_indexer_key_is_gated_spec_5_1) {
    auto r = runAot(prog("    using var d := new SortedDictionary<ulong, string>();\n"
                         "    d.Add(7, \"S\");\n"
                         "    var i := 7;\n"
                         "    Console.WriteLine(d[i]);\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the index argument to `SortedDictionary[key]` has a type that is "
           "not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(dict_indexer_key_6551,
           owning_dictionary_indexer_key_is_gated_spec_5_1) {
    auto r = runAot(prog("    using var d := new OwningDictionary<ulong, R6551>();\n"
                         "    d[7] := new R6551();\n"
                         "    var i := 7;\n"
                         "    Console.WriteLine(d[i].Tag);\n"
                         "    return 0;\n",
                    "public class R6551 public Tag: string := \"S\"; end R6551;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the index argument to `OwningDictionary[key]` has a type that is "
           "not implicitly convertible to the target type (§5.1)"));
}

// ── PARITY: the six members must now agree ────────────────────────────────
//
// Four negative rows pin the ALREADY-rejected siblings at the SAME non-implicit
// `int` key. They were green before this fix and must stay green: they are what
// makes "the indexer agrees with the other five" a claim a future weakening can
// falsify, in the direction #6527's four negative rows work. Each asserts the
// member-naming §5.1 wording, so a member silently dropping out of the gate
// turns the row red rather than leaving the parity untested.

BEZEL_TEST(dict_indexer_key_6551,
           parity_contains_key_still_rejected_spec_5_1) {
    auto r = runAot(prog("    var d: Dictionary<ulong, string> := "
                         "[7: \"S\"];\n"
                         "    var i := 7;\n"
                         "    Console.WriteLine(d.ContainsKey(i).ToString());\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the argument to `Dictionary<T>.ContainsKey` has a type that is "
           "not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(dict_indexer_key_6551,
           parity_get_or_null_still_rejected_spec_5_1) {
    auto r = runAot(prog("    var d: Dictionary<ulong, string> := "
                         "[7: \"S\"];\n"
                         "    var i := 7;\n"
                         "    Console.WriteLine(d.GetOrNull(i));\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the argument to `Dictionary<T>.GetOrNull` has a type that is "
           "not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(dict_indexer_key_6551,
           parity_in_operator_still_rejected_spec_5_1) {
    auto r = runAot(prog("    var d: Dictionary<ulong, string> := "
                         "[7: \"S\"];\n"
                         "    var i := 7;\n"
                         "    Console.WriteLine((i in d).ToString());\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the left operand of `in` over `Dictionary` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(dict_indexer_key_6551,
           parity_add_still_rejected_spec_5_1) {
    auto r = runAot(prog("    var d: Dictionary<ulong, string> := "
                         "[7: \"S\"];\n"
                         "    var i := 7;\n"
                         "    d.Add(i, \"Z\");\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the argument to `Dictionary<T>.Add` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
}

// ── The BARE TYPE PARAMETER fail-open — #6500's and #6527's precedent ─────
//
// §14.1 (*Instantiation and its site*) / §14.2 rule 2 read an interior use at
// the instantiations the declaration forms, not at the declaration, so a
// `Dictionary<T, V>` in a generic body has no key type to check at the
// declaration and must fail OPEN. MEASURED on the pre-fix base this compiled,
// and it still compiles — the carve-out is load-bearing, not decorative:
// closing it would newly reject a legal generic declaration.

BEZEL_TEST(dict_indexer_key_6551,
           generic_body_dictionary_key_fails_open_spec_14_2_rule_2) {
    BEZEL_EXPECT_EQ(
        std::string("907\n"),
        out("    Console.WriteLine(Helper.F<Range<int>>().ToString());\n"
            "    return 0;\n",
            "class Helper\n"
            "  public static method F<T>(): int\n"
            "    using var d := new Dictionary<T, int>();\n"
            "    d[1..5] := 7;\n"
            "    d[1..5 step 2] := 9;\n"
            "    var e: Range<int> := 1..5;\n"
            "    var a := d[1..5];\n"
            "    var b := d[e];\n"
            "    return a + b * 0 + 900 + (if e in d then 0 else 1000);\n"
            "  end F;\n"
            "end Helper;\n"));
}

// ── The NON-dictionary indexer families — NOW GATED (#7064) ────────────────
//
// These three rows were CONTROLS when this file was written: they pinned the
// scope boundary of #6551, which was scoped to the dictionary key slot, and
// they asserted that `Array<T>[j]` at a `long j`, `string[j]` at a `long j`
// and a user-declared `property [i: int]` at a `long j` all COMPILED AND RAN
// (exit 0).
//
// #7064 is that separate call, and it CLOSES the boundary: §23.4 makes an
// index argument a parameter pass, and §5.1 governs the conversion at that
// pass. `Array<T>`'s indexer declares `[i: int]` (stdlib §4.4), `string`'s
// is the same `[i: int]` position (spec §4.6), and a user-declared
// `property [i: int]` declares its own `int` — so a `long` argument has no
// §5.1 row at any of them (§5.1's table row is `int` -> `long`, never the
// reverse, and §5.1's constant-narrowing clause — the only way a wider
// integer reaches a narrower declared type — applies to COMPILE-TIME
// CONSTANTS only). The comment above the old rows said exactly this: "`Array
// <T>[j]` at a `long j` is ALSO a §5.1 error (`long -> int` is a narrowing,
// and §5.1's table has no such row)".
//
// BEHAVIOUR FLIP: these three rows changed from "compiles and runs" to
// "rejected with the §5.1 wording". The pre-#7064 expectation is not
// preserved anywhere: the §5.1 error they pinned was never a §5.1-legal
// program.

BEZEL_TEST(dict_indexer_key_6551,
           array_indexer_long_index_is_rejected_spec_5_1) {
    auto r = runAot(prog("    var a: Array<int> := [1, 2, 3];\n"
                         "    var j: long := 1;\n"
                         "    Console.WriteLine(a[j].ToString());\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the index argument to `Array[i]` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(dict_indexer_key_6551,
           string_indexer_long_index_is_rejected_spec_5_1) {
    auto r = runAot(prog("    var s := \"abc\";\n"
                         "    var j: long := 1;\n"
                         "    Console.WriteLine(s[j].ToString());\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the index argument to `string[i]` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
}

// A USER-declared indexer (`property [i: int]`, §23.4) reaches the same
// position and is gated by the same §5.1 rule: the parameter type is the
// indexer's OWN declaration, and §5.1 decides whether the argument is
// admitted at it.
BEZEL_TEST(dict_indexer_key_6551,
           user_indexer_long_index_is_rejected_spec_5_1) {
    auto r = runAot(prog("    using var b := new R6551Box();\n"
                         "    var j: long := 1;\n"
                         "    Console.WriteLine(b[j].ToString());\n"
                         "    return 0;\n",
                         "public class R6551Box\n"
                         "  private _v: Array<int> := [1, 2, 3];\n"
                         "  public property [i: int]: int\n"
                         "    get => _v[i];\n"
                         "    set => do _v[i] := value; end;\n"
                         "  end;\n"
                         "end R6551Box;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the index argument to `R6551Box[i]` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
}