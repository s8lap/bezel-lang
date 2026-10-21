// fix #6527 (§5.1, v0.338; §17.5, v0.338; stdlib §4.8/§4.9/§4.17): a range
// (or any other non-convertible value) in a dictionary's KEY slot is rejected
// at `GetOrNull`, `TryGetValue` and `in` — the same §5.1 argument-conversion
// check `ContainsKey` / `Contains` / `Remove` and the indexer already reach.
//
// THE SPEC. Two independent clauses, each sufficient on its own:
//
//   * §5.1 (v0.338) enumerates the implicit conversions, and the table is
//     NUMERIC WIDENING only. There is no row converting a `Range<int>` (or a
//     `double`, or a `bool`) to an `int` key, so such an argument is a compile
//     error at the conversion site — never a silent truncation. stdlib §4.8
//     declares `GetOrNull(key: K): V?` and `TryGetValue(key: K, out value: V?):
//     bool` with `K` exactly as it declares `ContainsKey(key: K): bool`, so the
//     three take the same argument through the same §5.1 gate.
//   * §17.5 (v0.338) makes `key in m` the structural `Contains(key)` lookup, and
//     stdlib §4's *Key membership* row says so in the stdlib's own words. So
//     `0..1 in d` is exactly `d.Contains(0..1)`, and whatever that call's
//     verdict is, `in`'s is the same.
//
// WHAT THE PRE-FIX COMPILER DID, and why it is the expensive kind of defect.
// The argument-conversion check is `checkBuiltinCollectionCallArgs`, and it is
// keyed on a `CallExpr`. `in` is a `BinaryExpr` that lowers to no call, so its
// left operand was never checked against the key type at all; and `GetOrNull` /
// `TryGetValue` were simply absent from that helper's list of key-position
// methods. The result is not a rejection and not a throw — a SILENTLY WRONG
// READ at exit 0, because the int-key encoder takes the value's first storage
// word as the key. MEASURED on the pre-fix base, exit 0, no diagnostic:
//     var d: Dictionary<int, string> := [0: "AA"];
//     d.GetOrNull(0..1)              ->  [AA]   (should be a compile error)
//     d.GetOrNull(3.5)               ->  [AA]   (should be a compile error)
//     d.TryGetValue(0..1, out v)     ->  ok=True, v=AA
//     0..1 in d                      ->  True   (should be a compile error)
// while on the SAME dictionary, in the SAME program:
//     d.ContainsKey(0..1)  and  d.Contains(3.5)  ->  both REJECTED (§5.1)
// Five ways of asking one question about one absent key; two reject, three
// answer confidently and wrongly. That is the top defect class in this project:
// a caller writing `GetOrNull` precisely to be safe against a missing key gets
// a wrong non-null value, and a membership test reports a key present that the
// same type's `ContainsKey` refuses to compile.
//
// NOT RANGE-SPECIFIC. The issue frames the gap as "a range argument into a
// non-range key slot", and the range is the reported trigger, but the mechanism
// is the missing §5.1 gate itself — so `3.5 in d` and `d.GetOrNull(3.5)` were
// wrong the same way and are covered here for the same reason. A fix that
// matched on the `Range` SPELLING would leave every one of those reading a wrong
// value; the rows below pin the mechanism, not the trigger.
//
// All rows are on the `runAot` oracle (the in-process JIT harness is
// deprecated), and every rejection row asserts the §5.1 WORDING as well as the
// rejection — `failedAtCompileTimeWithRealLocation` alone is satisfied by a
// crash or an IR-verification failure, which is exactly what two of these
// programs produced before the fix.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"  // runAot
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& mainBody, const std::string& decls = "") {
    return "module M;\n" + decls +
           "class C\n  public static method Main(): int\n" + mainBody +
           "  end Main;\nend C;\n";
}

// The rejection oracle. `r.ran == false` alone is satisfied by a crash, a
// signal, a timeout, an ASan report and an IR-verification failure alike, so it
// discriminates nothing on its own; the WORDING does.
bool rejectedWith(const JitResult& r, const char* msg) {
    return failedAtCompileTimeWithRealLocation(r) &&
           diagnosticMessageContains(r, msg);
}

// stdout of a program that must compile, run and exit 0. `ran == false` reads
// back as a marker no expectation equals, so a rejection FAILS an accept row
// rather than passing it.
std::string out(const std::string& mainBody, const std::string& decls = "") {
    auto r = runAot(prog(mainBody, decls));
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// ── `GetOrNull` ────────────────────────────────────────────────────────────

// The issue's own row, verbatim. Pre-fix this COMPILED, RAN and printed `[AA]`:
// the value under key `0`, read through a range that names no key at all. This
// is the row that makes the fix load-bearing — it is satisfied by nothing on the
// pre-fix compiler.
BEZEL_TEST(dict_range_key_6527,
           get_or_null_range_key_was_silently_reading_key_zero_spec_5_1_stdlib_4_8) {
    auto r = runAot(prog("    var d: Dictionary<int, string> := [0: \"AA\"];\n"
                         "    Console.WriteLine(d.GetOrNull(0..1));\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the argument to `Dictionary<T>.GetOrNull` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
}

// The same gap with a non-range trigger, which is the shape a fix keyed on the
// `Range` SPELLING would miss: §5.1's table is numeric widening, so a `double`
// is no more admissible in an `int` key slot than a range is. Pre-fix this also
// compiled, ran and printed `[AA]` — the same wrong read.
BEZEL_TEST(dict_range_key_6527,
           get_or_null_double_key_is_not_a_widening_spec_5_1_stdlib_4_8) {
    auto r = runAot(prog("    var d: Dictionary<int, string> := [0: \"AA\"];\n"
                         "    Console.WriteLine(d.GetOrNull(3.5));\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the argument to `Dictionary<T>.GetOrNull` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
}

// A `Range` VALUE rather than a range literal — `d.GetOrNull(r)` over
// `var r: Range<int> := 0..1`. The literal spelling and the typed value are
// distinct AST nodes; a fix that inspected only the syntax would leave this one
// to the encoder, which reads the same wrong word.
BEZEL_TEST(dict_range_key_6527,
           get_or_null_range_value_key_rejects_spec_5_1_spec_16_4) {
    auto r = runAot(prog("    var d: Dictionary<int, string> := [0: \"AA\"];\n"
                         "    var r: Range<int> := 0..1;\n"
                         "    Console.WriteLine(d.GetOrNull(r));\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the argument to `Dictionary<T>.GetOrNull` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
}

// A `string` argument to an `int` key. §5.1 has no row for it either, and the
// pre-fix spelling returned `null` (the string-key encoder hashed a pointer) —
// a right answer for a program that should not have compiled, which is why the
// range and double rows above rather than this one carry the "wrong value"
// claim.
BEZEL_TEST(dict_range_key_6527,
           get_or_null_string_key_on_int_dict_rejects_spec_5_1) {
    auto r = runAot(prog("    var d: Dictionary<int, string> := [0: \"AA\"];\n"
                         "    Console.WriteLine(d.GetOrNull(\"x\"));\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the argument to `Dictionary<T>.GetOrNull` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
}

// ── `TryGetValue` ──────────────────────────────────────────────────────────

// stdlib §4.8 declares `TryGetValue(key: K, out value: V?): bool` with the same
// `key: K` slot `ContainsKey` declares, so the same §5.1 gate applies. Pre-fix
// it compiled, ran and reported SUCCESS on an absent key — the sharpest form of
// the defect, since the caller's entire reason for writing `Try*` is that it can
// trust the `false`.
BEZEL_TEST(dict_range_key_6527,
           try_get_value_range_key_reports_success_wrongly_spec_5_1_stdlib_4_8) {
    auto r = runAot(prog("    var d: Dictionary<int, string> := [0: \"AA\"];\n"
                         "    var ok := d.TryGetValue(0..1, out var v);\n"
                         "    Console.WriteLine($\"{ok} {v}\");\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the argument to `Dictionary<T>.TryGetValue` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
}

// The non-range trigger on the same member. Pre-fix: `ok=True v=AA`.
BEZEL_TEST(dict_range_key_6527,
           try_get_value_double_key_reports_success_wrongly_spec_5_1) {
    auto r = runAot(prog("    var d: Dictionary<int, string> := [0: \"AA\"];\n"
                         "    var ok := d.TryGetValue(3.5, out var v);\n"
                         "    Console.WriteLine($\"{ok} {v}\");\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the argument to `Dictionary<T>.TryGetValue` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
}

// ── `in` ───────────────────────────────────────────────────────────────────

// §17.5 (v0.338) + stdlib §4's *Key membership* row: `key in m` IS
// `m.Contains(key)`. `d.Contains(0..1)` is rejected on this same compiler, so
// `(0..1 in d)` cannot be `True`. Pre-fix it compiled, ran and printed `True`.
BEZEL_TEST(dict_range_key_6527,
           in_range_left_operand_is_rejected_like_contains_spec_17_5_spec_5_1) {
    auto r = runAot(prog("    var d: Dictionary<int, string> := [0: \"AA\"];\n"
                         "    Console.WriteLine((0..1 in d).ToString());\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the left operand of `in` over `Dictionary` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
}

// The non-range trigger on `in`. Pre-fix `(3.5 in d)` printed `True`.
BEZEL_TEST(dict_range_key_6527,
           in_double_left_operand_is_rejected_like_contains_spec_17_5) {
    auto r = runAot(prog("    var d: Dictionary<int, string> := [0: \"AA\"];\n"
                         "    Console.WriteLine((3.5 in d).ToString());\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the left operand of `in` over `Dictionary` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
}

// THE PARITY ROW, and the one that states the issue's thesis as an assertion:
// the two spellings of the SAME question must give the SAME verdict. §17.5
// makes them one question, so if the call form is a compile error the operator
// form is too. Pre-fix, `d.Contains(0..1)` was rejected while `(0..1 in d)`
// returned `True` in the same program — asserted here as the pair.
BEZEL_TEST(dict_range_key_6527,
           in_and_contains_agree_on_the_absent_key_spec_17_5_spec_5_1) {
    auto call = runAot(prog("    var d: Dictionary<int, string> := [0: \"AA\"];\n"
                            "    Console.WriteLine(d.Contains(0..1).ToString());\n"
                            "    return 0;\n"));
    auto oper = runAot(prog("    var d: Dictionary<int, string> := [0: \"AA\"];\n"
                            "    Console.WriteLine((0..1 in d).ToString());\n"
                            "    return 0;\n"));
    const char* needle =
        "has a type that is not implicitly convertible to the target type "
        "(§5.1)";
    // Both are rejections, and both name §5.1 — the operator form does not get
    // to answer `True` while the call form refuses to compile.
    BEZEL_EXPECT_TRUE(rejectedWith(call, needle));
    BEZEL_EXPECT_TRUE(rejectedWith(oper, needle));
}

// ── The other two keyed families ────────────────────────────────────────────

// stdlib §4.9's OWNING sibling. §4.9 declares the same `GetOrNull(key: K): V?`
// on `OwningDictionary<K, V>`, and `OwningDictionary` is in the same `K`-slot
// list the check enumerates, so this is covered by the same code as the rows
// above rather than by a name test.
BEZEL_TEST(dict_range_key_6527,
           owning_dictionary_get_or_null_range_key_rejects_stdlib_4_9) {
    auto r = runAot(prog("    using var d := new OwningDictionary<int, "
                         "Box>();\n"
                         "    d.Add(0, new Box(\"AA\"));\n"
                         "    var g := d.GetOrNull(0..1);\n"
                         "    Console.WriteLine($\"{g = null}\");\n"
                         "    return 0;\n",
                         "class Box\n  public V: string;\n"
                         "  public constructor(v: string) V := v; end "
                         "constructor;\nend Box;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the argument to `OwningDictionary<T>.GetOrNull` has a type that is "
           "not implicitly convertible to the target type (§5.1)"));
}

// stdlib §4.17's SORTED sibling, same declared surface (§4.17 documents the
// point operations `[k]`, `ContainsKey`, `TryGetValue`, `GetOrNull`, `Remove`
// together), and `in` over it.
BEZEL_TEST(dict_range_key_6527,
           sorted_dictionary_get_or_null_and_in_reject_stdlib_4_17) {
    auto g = runAot(prog("    var d := new SortedDictionary<int, string>();\n"
                         "    d.Add(0, \"AA\");\n"
                         "    Console.WriteLine(d.GetOrNull(0..1));\n"
                         "    return 0;\n"));
    auto i = runAot(prog("    var d := new SortedDictionary<int, string>();\n"
                         "    d.Add(0, \"AA\");\n"
                         "    Console.WriteLine((0..1 in d).ToString());\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        g, "the argument to `SortedDictionary<T>.GetOrNull` has a type that is "
           "not implicitly convertible to the target type (§5.1)"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        i, "the left operand of `in` over `SortedDictionary` has a type that "
           "is not implicitly convertible to the target type (§5.1)"));
}

// ── NEGATIVES: the already-rejected siblings must stay rejected ─────────────
//
// Each of these was ALREADY rejected before this change, by the §5.1
// argument-conversion check this fix extends to three more members. They are
// here so a future change to that check cannot quietly regress a rejection into
// acceptance. They pin the SPECIFIC diagnostic (which names the member), not
// merely "did not compile": a fix that removed the check wholesale would leave
// these compiling, and these rows would catch it.

// `ContainsKey` — the member whose check the other three now share.
BEZEL_TEST(dict_range_key_6527,
           negative_contains_key_range_arg_stays_rejected_spec_5_1) {
    auto r = runAot(prog("    var d: Dictionary<int, string> := [0: \"AA\"];\n"
                         "    Console.WriteLine(d.ContainsKey(0..1).ToString());\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the argument to `Dictionary<T>.ContainsKey` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
}

// `Contains` — §17.5's call spelling, the one `in` must agree with.
BEZEL_TEST(dict_range_key_6527,
           negative_contains_range_arg_stays_rejected_spec_17_5_spec_5_1) {
    auto r = runAot(prog("    var d: Dictionary<int, string> := [0: \"AA\"];\n"
                         "    Console.WriteLine(d.Contains(0..1).ToString());\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the argument to `Dictionary<T>.Contains` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
}

// `Remove` — the third sibling named in the issue's matrix.
BEZEL_TEST(dict_range_key_6527,
           negative_remove_range_arg_stays_rejected_spec_5_1_stdlib_4_8) {
    auto r = runAot(prog("    var d: Dictionary<int, string> := [0: \"AA\"];\n"
                         "    Console.WriteLine(d.Remove(0..1).ToString());\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "the argument to `Dictionary<T>.Remove` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
}

// The INDEXER — rejected by #6500, a SEPARATE check (`ExprTypecheck_Exprs.cpp`'s
// indexer branch, keyed on the key TYPE). Pins that #6500's rejection is still
// here; its diagnostic differs from the §5.1 one, which is why it is asserted
// separately.
BEZEL_TEST(dict_range_key_6527,
           negative_range_indexer_stays_rejected_spec_23_4_spec_15_6) {
    auto r = runAot(prog("    var d: Dictionary<int, string> := [0: \"AA\"];\n"
                         "    Console.WriteLine(d[0..1]);\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "range index `[a..b]` is not admitted on `Dictionary<K, V>`"));
}

// ── ACCEPT controls: the boundary the check must not cross ──────────────────

// stdlib §9.8 + spec §8.3, v0.338: `Range<T>` is a compiler-intrinsic record
// with §8.3's auto-hashable surface, so `Dictionary<Range<int>, V>` is a legal
// key type and these are ordinary key lookups. This is the one shape none of the
// rejections above may take, and #6500 measured it as ALREADY CORRECT before
// this change — every form below returned the stored value / `null` / `True` as
// it should. MEASURED on the pre-fix base, so these rows pin "still correct"
// rather than "newly correct".
//
// Three keys sharing a `Start` and differing in the other storage words
// (§16.3's `IsEndInclusive`, §16.4's `Step`) so the row is not satisfiable by
// anything that consults only the first word — the exact reading that made
// `Dictionary<int, V>` wrong. `7 + 80 + 900 = 987`.
BEZEL_TEST(dict_range_key_6527,
           range_keyed_dict_all_three_forms_still_look_the_key_up_spec_8_3_stdlib_9_8) {
    BEZEL_EXPECT_EQ(
        std::string("987\n987\nTrue\nTrue\n"),
        out("    using var d := new Dictionary<Range<int>, int>();\n"
            "    d[1..5] := 7;\n"
            "    d[1..<5] := 8;\n"
            "    d[1..5 step 2] := 9;\n"
            "    var x := d.GetOrNull(1..5);\n"
            "    var y := d.GetOrNull(1..<5);\n"
            "    var z := d.GetOrNull(1..5 step 2);\n"
            "    Console.WriteLine((x! + y! * 10 + z! * 100).ToString());\n"
            "    var e: Range<int> := 1..5;\n"
            "    var ok := d.TryGetValue(e, out var g);\n"
            "    Console.WriteLine(((if ok then g! else 0) + "
            "(if d.TryGetValue(1..<5, out var h) then h! * 10 else 0) + "
            "(if d.TryGetValue(1..5 step 2, out var j) then j! * 100 else 0))"
            ".ToString());\n"
            "    Console.WriteLine((1..5 in d).ToString());\n"
            "    Console.WriteLine($\"{d.GetOrNull(7..9) = null}\");\n"
            "    return 0;\n"));
}

// A `Range` KEY whose absent case must still answer `null` / `false`, not a
// wrong value — the read semantics stdlib §4.8 states for `GetOrNull` and
// §4's *Key membership* row states for `in`. Pins that the accept path returns
// the RIGHT answer and not merely "compiles".
BEZEL_TEST(dict_range_key_6527,
           range_keyed_dict_absent_key_reads_as_absent_stdlib_4_8_spec_17_5) {
    BEZEL_EXPECT_EQ(
        std::string("True\n0 1\nFalse\n"),
        out("    using var d := new Dictionary<Range<int>, int>();\n"
            "    d[1..5] := 7;\n"
            "    Console.WriteLine($\"{d.GetOrNull(7..9) = null}\");\n"
            "    var ok := d.TryGetValue(7..9, out var v);\n"
            "    Console.WriteLine($\"{(if ok then 1 else 0)} "
            "{(if v = null then 1 else 0)}\");\n"
            "    Console.WriteLine((7..9 in d).ToString());\n"
            "    return 0;\n"));
}

// The ordinary `int` key — the shape adjacent to every rejected row. Each
// member must still READ its own entry and report membership honestly, so a fix
// that rejected on "the argument is not the key type" too eagerly (or that
// fired on any `in`) fails here.
BEZEL_TEST(dict_range_key_6527,
           control_scalar_key_all_three_forms_are_unchanged_spec_5_1_stdlib_4_8) {
    BEZEL_EXPECT_EQ(
        std::string("AA\nTrue AA\nTrue\nFalse\n"),
        out("    var d: Dictionary<int, string> := [0: \"AA\"];\n"
            // #6778 (§7.1, §18.5, v0.340): the `string?` result prints
            // through a hole at `WriteLine`'s non-nullable parameter.
            "    Console.WriteLine($\"{d.GetOrNull(0)}\");\n"
            "    var ok := d.TryGetValue(0, out var v);\n"
            "    Console.WriteLine($\"{ok} {v}\");\n"
            "    Console.WriteLine((0 in d).ToString());\n"
            "    Console.WriteLine((7 in d).ToString());\n"
            "    return 0;\n"));
}

// §5.1's CONSTANT-NARROWING clause must keep applying through the new `in` gate,
// exactly as it does through the call form: an integer CONSTANT converts to any
// integer type whose range contains its value, so `3 in s` over a `Set<byte>` —
// and, for the keyed case this fix adds, a `byte` key — is admitted, not
// rejected as a widening violation.
BEZEL_TEST(dict_range_key_6527,
           constant_narrowing_still_admitted_through_in_spec_5_1) {
    BEZEL_EXPECT_EQ(
        std::string("True\n"),
        out("    var d := new Dictionary<byte, string>();\n"
            "    d.Add(3, \"three\");\n"
            "    Console.WriteLine((3 in d).ToString());\n"
            "    return 0;\n"));
}

// A BARE TYPE PARAMETER key — `Dictionary<T, V>` in a generic body — is
// undecidable at the DECLARATION: at `T := Range<int>` the lookup below is a
// §8.3 key lookup and at `T := int` it would be inadmissible, and spec §14.2
// rule 2 reads the body's own comparison at each instantiation. So the check
// must FAIL OPEN there, as #6500's indexer guard does.
//
// Instantiated at `Range<int>` and READ, so the row is satisfiable only by a
// check that really declined to judge the key type.
BEZEL_TEST(dict_range_key_6527,
           bare_type_parameter_key_body_is_left_alone_spec_14_2) {
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
            "    var x := d.GetOrNull(1..5);\n"
            "    var ok := d.TryGetValue(e, out var y);\n"
            "    var z := d.GetOrNull(1..5 step 2);\n"
            "    return x! + y! * 0 + z! * 100 + (if e in d then 0 else "
            "1000);\n"
            "  end F;\n"
            "end Helper;\n"));
}

// §15.6's ADMITTED range slice — `Array<T>` — must still slice. The rejection is
// on the DICTIONARY KEY SLOT, not on the index form; the `in` gate added here is
// likewise keyed on the receiver, so `a` must keep slicing and keep admitting a
// range-shaped expression of its own.
BEZEL_TEST(dict_range_key_6527,
           control_array_range_slice_is_unchanged_spec_15_6) {
    BEZEL_EXPECT_EQ(
        std::string("2\n"),
        out("    var a: Array<int> := [10, 20, 30];\n"
            "    var s := a[0..1];\n"
            "    Console.WriteLine(s.Count.ToString());\n"
            "    return 0;\n"));
}

// `in` over an ELEMENT collection is deliberately NOT gated by this change (the
// fix's scope is the keyed families; the element families admit a wider
// legitimate left-operand set through §8.4 equality). This row pins that
// boundary so a future widening of the `in` gate is a conscious act: ordinary
// membership over an `Array` and over a `Range` must keep working, spelled both
// ways.
BEZEL_TEST(dict_range_key_6527,
           control_in_over_element_collections_is_unchanged_spec_17_5_stdlib_9_8) {
    BEZEL_EXPECT_EQ(
        std::string("True\nFalse\nTrue\nTrue\n"),
        out("    var a: Array<int> := [1, 2];\n"
            "    Console.WriteLine((1 in a).ToString());\n"
            "    Console.WriteLine((7 in a).ToString());\n"
            "    Console.WriteLine((3 in 1 .. 5).ToString());\n"
            "    var s: Set<int> := [1, 2];\n"
            "    Console.WriteLine((2 in s).ToString());\n"
            "    return 0;\n"));
}