// Codegen — issue #6541: a §5.1 widening ADMITTED at a generic collection key
// slot is never MATERIALISED, so the runtime hashes `key_size` bytes off a
// narrower stack slot (a stack-buffer-overflow) and the members then disagree
// about the same key.
//
// WHAT WAS ACTUALLY WRONG (measured on base `e0594ff4`, not assumed). The
// generic-key runtime reads `d->key_size` bytes off the pointer codegen hands
// it — `bz_dict_contains_gen` / `bz_dict_try_get_gen` / `bz_dict_set_gen`, all
// funnelling through `bz$valkey$hash` — while every call site spills `k` at
// `k->getType()`, the SOURCE expression's width, which `lowerExpr` derives from
// the literal/local rather than from the key slot. §5.1's table admits
// `int` → `long` / `float` / `double` / `decimal`, so at `Dictionary<long, V>`
// the `7` is a legal key whose slot is 8 bytes wide:
//
//     ==2905632==ERROR: AddressSanitizer: stack-buffer-overflow
//       READ of size 8 ... partially overflows this variable
//         #0 bz$valkey$hash
//         #1 bz_dict_try_get_gen bezel_runtime_collections.cpp:1182
//
// The ISSUE's framing — "the indexer widens correctly; GetOrNull and
// ContainsKey do not" — is a SNAPSHOT OF STACK GARBAGE, and MEASURED evidence
// says the split runs the other way and moves between runs. On one base, with
// an `int` LOCAL as the key, `ContainsKey` / `in` / the indexer / `Remove`
// answered present while `GetOrNull` answered null; with the LITERAL `7` all
// five answered absent. ASan shows all five over-reading the same slot. So this
// is ONE site class, not five, and the four-byte question was never which member
// widens — it was what happened to sit above the spill.
//
// §5.1 is the clause that decides the answer: the conversion is ADMITTED at the
// key slot, so the entry stored as `Add(7L, "K")` and the lookup spelled `7` are
// the same key, and §4.8's `GetOrNull(key: K): V?` — "absent → null" — must
// answer `K`. stdlib §4.8 also makes `key in dict` equivalent to
// `dict.ContainsKey(key)` (spec §17.5), so `in` must agree with `ContainsKey`
// about the same key; pre-fix, on the `ulong` row, they did not.
//
// THE FIX — `Emitter::coerceDictKeyToKeyType` (LlvmBackend_impl.hpp, beside
// `loadStringData`), applied at every generic-key spill site:
//
//   * `genKeyPtr` (LlvmBackend_Calls.cpp) — the ONE lambda every NAMED member
//     of the isDict block shares: ContainsKey/Contains/Add/TryAdd/Remove/
//     TryGetValue/GetOrNull/GetOrAdd;
//   * `elemPtr` (LlvmBackend_Calls.cpp) — a `Set<T>` is the same `bz_dict_t*`
//     table, so its ELEMENT is this table's KEY;
//   * `key in dict` and `elem in set` (LlvmBackend_Exprs.cpp);
//   * the indexer GET, `move d[k]`, and the indexer SET (LlvmBackend_Assign.cpp
//     ×3);
//   * the dictionary-literal materializer (LlvmBackend_Setup.cpp).
//
// §5.1's CONSTANT NARROWING is the mirror (`Dictionary<byte, V>` read with the
// constant `5`) and is MEASURED already correct — `lowerExpr` lowers a constant
// to the narrowed width — so no Trunc is emitted: a non-constant narrowing is
// on §5.1's never-implicit list and is rejected in sema before codegen.
//
// Oracles: `runAot` throughout, asserting PRINTED VALUES. An accept-shaped
// assertion ("it compiled") passes on today's wrong output, which is why every
// value row prints what each member ANSWERED.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string out(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did-not-run>";
    return r.stdoutText;
}

std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

bool sanitizerClean(const std::string& program) {
    auto r = runAotChecked(program);
    if (!r.ran) return false;
    return !r.diagnostics.hasErrors();
}

// The five members that ask about a key, printed in a fixed order so a row can
// name exactly which one disagreed. `key in d` is inside its own `var` because
// §17.5's operator does not parse inside a string interpolation directly.
std::string fiveMembers(const std::string& decl, const std::string& add,
                        const std::string& lit) {
    return "var d := new " + decl + "(); "
           "d.Add(" + add + ", \"K\"); "
           "var g := d.GetOrNull(" + lit + "); "
           "Console.WriteLine($\"G={(if g = null then \"null\" else g!)} \"); "
           "Console.WriteLine($\"C={d.ContainsKey(" + lit + ").ToString()} \"); "
           "var q := " + lit + " in d; "
           "Console.WriteLine($\"I={q.ToString()} \"); "
           "if d.ContainsKey(" + lit + ") then "
           "  var x := d[" + lit + "]; "
           "  Console.WriteLine($\"X={x!} \"); "
           "else "
           "  Console.WriteLine(\"X=absent \"); "
           "end if; "
           "Console.WriteLine($\"R={d.Remove(" + lit + ").ToString()}\"); ";
}

// Every member found it and then unlinked it — the whole surface agrees.
const char kAllFound[] =
    "G=K \nC=True \nI=True \nX=K \nR=True\n";

}  // namespace

// ── The issue's own repro, verbatim in shape ───────────────────────────────
// `Dictionary<long, string>` with `Add(5L, "L5")`. MEASURED on the base the
// issue reports (`GetOrNull(5)`=null, `[5]`=`L5`, `ContainsKey(5)`=False) —
// and MEASURED too that the split is not stable, so this row pins what §5.1 +
// §4.8 REQUIRE (all five agree the entry is there), not which spelling
// happened to win on the reporter's stack.
BEZEL_TEST(dict_widened_key_6541,
           dict_long_key_read_back_with_int_literal_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var ld := new Dictionary<long, string>(); "
            "ld.Add(5L, \"L5\"); "
            "Console.WriteLine($\"Count={ld.Count.ToString()}\"); "
            "var a := ld.GetOrNull(5); "
            "Console.WriteLine($\"GetOrNull(5)={(if a = null then \"null\" "
            "else a!)}\"); "
            "var b := ld[5]; "
            "Console.WriteLine($\"index[5]={(if b = null then \"null\" "
            "else b!)}\"); "
            "Console.WriteLine($\"ContainsKey(5)={"
            "ld.ContainsKey(5).ToString()}\"); "
            "var c := ld.GetOrNull(5L); "
            "Console.WriteLine($\"GetOrNull(5L)={(if c = null then \"null\" "
            "else c!)}\"); ")),
        "Count=1\nGetOrNull(5)=L5\nindex[5]=L5\nContainsKey(5)=True\n"
        "GetOrNull(5L)=L5\n");
}

// The same repro with a NON-constant `int` key. §5.1 admits `int` → `long`
// for a run-time value too, and on the base this spelling is the one where
// MEASURED `GetOrNull` answered null while the other four answered present —
// the issue's own "the indexer widens correctly" observation, and the reason
// the split is stack garbage rather than a per-member rule.
BEZEL_TEST(dict_widened_key_6541,
           dict_long_key_read_back_with_int_variable_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out(prog("var i := 7; " +
                 fiveMembers("Dictionary<long, string>", "7L", "i"))),
        kAllFound);
}

// ── Every §5.1-admitted widening at a key slot, one row each ───────────────
// A matrix rather than a single row, because the trigger is a PAIR of types and
// a fix written for `int` → `long` alone would leave `double` untouched.

BEZEL_TEST(dict_widened_key_6541, dict_double_key_widened_from_int_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out(prog("var i := 7; " +
                 fiveMembers("Dictionary<double, string>", "7.0", "i"))),
        kAllFound);
}

BEZEL_TEST(dict_widened_key_6541, dict_float_key_widened_from_int_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out(prog("var i := 7; " +
                 fiveMembers("Dictionary<float, string>", "7.0f", "i"))),
        kAllFound);
}

// `int` -> `ulong` is NOT on §5.1's table (`int` widens to `long`, `float`,
// `double`, `decimal`), and this row's source is a NON-CONSTANT, so §5.1's
// constant-narrowing clause does not reach it either: the program is a compile
// error. MEASURED on the base and on this tip — the wording names §5.1 — so
// the row pins that the widening fix did not make the §5.1 gate permissive.
//
// The assertion is the STRONG one, not a bare `!r.ran`: `runAot`'s `ran` is
// false for a crash, a signal, a timeout and a forced ASan report just as it
// is for a genuine rejection (issue #3892), so this row pins BOTH that the
// driver rejected with a REAL source location and that the diagnostic is the
// §5.1 one by name. #6527 (`3ac61606`, an ancestor of this tip) is what put
// the `GetOrNull` / `in` sites under the same gate as the named members;
// MEASURED on this tip, all five members of `fiveMembers` reject here.
BEZEL_TEST(dict_widened_key_6541,
           dict_int_to_ulong_non_constant_still_rejected_spec_5_1) {
    auto r = runAot(prog("var i := 7; " +
                         fiveMembers("Dictionary<ulong, string>", "7UL", "i")));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type (§5.1)"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "between signed and unsigned types of the same or narrower width is "
           "never implicit"));
}

// An `ulong`-typed source at a `ulong` key: the exact-width control for the
// zero-extend direction `coerceNumericToType` picks from source signedness.
BEZEL_TEST(dict_widened_key_6541,
           dict_ulong_key_exact_width_control_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out(prog("var u := 7UL; " +
                 fiveMembers("Dictionary<ulong, string>", "7UL", "u"))),
        kAllFound);
}

BEZEL_TEST(dict_widened_key_6541,
           dict_ulong_key_widened_from_int_literal_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out(prog(fiveMembers("Dictionary<ulong, string>", "7UL", "7"))),
        kAllFound);
}

// A §5.1 float → double widening (the one binary-float row of the table),
// which takes the FPExt arm rather than the integer arm.
BEZEL_TEST(dict_widened_key_6541, dict_double_key_widened_from_float_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out(prog("var f := 7.0f; " +
                 fiveMembers("Dictionary<double, string>", "7.0", "f"))),
        kAllFound);
}

// ── The other receivers that share the one encoder ─────────────────────────

// `Set<T>` is the same `bz_dict_t*` table, so its ELEMENT is this table's KEY
// and the identical over-read applies (`elemPtr`, LlvmBackend_Calls.cpp).
BEZEL_TEST(dict_widened_key_6541, set_long_element_widened_from_int_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var s := new Set<long>(); "
            "var i := 7; "
            "s.Add(i); "
            "Console.WriteLine($\"Count={s.Count.ToString()} \"); "
            "Console.WriteLine($\"C={s.Contains(i).ToString()} \"); "
            "var q := i in s; "
            "Console.WriteLine($\"I={q.ToString()} \"); "
            "Console.WriteLine($\"R={s.Remove(i).ToString()}\"); ")),
        "Count=1 \nC=True \nI=True \nR=True\n");
}

// `SortedDictionary<K, V>` rides the same `isDict` block (stdlib §4.17).
BEZEL_TEST(dict_widened_key_6541,
           sorted_dict_long_key_widened_from_int_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out(prog("var i := 7; " +
                 fiveMembers("SortedDictionary<long, string>", "7L", "i"))),
        kAllFound);
}

// The dictionary LITERAL is the store-side twin (LlvmBackend_Setup.cpp's
// `spillKey`), and reaches the runtime through `bz_dict_set_gen` rather than
// the lookup entry points — so it is its own row.
BEZEL_TEST(dict_widened_key_6541,
           dict_literal_widened_key_is_found_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var d: Dictionary<long, string> := [7: \"LIT\"]; "
            "var i := 7; "
            "Console.WriteLine($\"C={d.ContainsKey(i).ToString()} \"); "
            "var g := d.GetOrNull(i); "
            "Console.WriteLine($\"G={(if g = null then \"null\" else g!)} \"); "
            "var x := d[i]; "
            "Console.WriteLine($\"X={x!}\"); ")),
        "C=True \nG=LIT \nX=LIT\n");
}

// The indexer SET is the fourth distinct spill site (Assign.cpp's
// `lowerCollectionIndexerSet`): `d[7] := v` at `Dictionary<long, V>` reaches
// `bz_dict_set_gen`'s `memcpy(kc, key, key_size)` — the same over-read #4640
// recorded for the Object-keyed shape, on the widening axis.
BEZEL_TEST(dict_widened_key_6541,
           dict_indexer_set_widened_key_then_lookup_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var d := new Dictionary<long, string>(); "
            "var i := 7; "
            "d[i] := \"SET\"; "
            "Console.WriteLine($\"C={d.ContainsKey(i).ToString()} \"); "
            "var g := d.GetOrNull(i); "
            "Console.WriteLine($\"G={(if g = null then \"null\" else g!)}\"); ")),
        "C=True \nG=SET\n");
}

// ── Controls: shapes that were already correct must be untouched ───────────

// A key of the KEY TYPE's own width — the overwhelmingly common case, and the
// one a sloppy fix (an unconditional widening, or a Trunc on every integer key)
// would break.
BEZEL_TEST(dict_widened_key_6541, dict_exact_width_key_control_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out(prog("var i := 7L; " +
                 fiveMembers("Dictionary<long, string>", "7L", "i"))),
        kAllFound);
}

// §5.1's CONSTANT NARROWING (`Dictionary<byte, V>` read with the constant 5)
// is the mirror of the defect and is already correct — pinning it is what makes
// "no Trunc was added" checkable.
BEZEL_TEST(dict_widened_key_6541,
           dict_constant_narrowed_key_control_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out(prog(fiveMembers("Dictionary<byte, string>", "5", "5"))),
        kAllFound);
}

BEZEL_TEST(dict_widened_key_6541,
           dict_constant_narrowed_key_control_short_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out(prog(fiveMembers("Dictionary<short, string>", "5", "5"))),
        kAllFound);
}

// A NEGATIVE widened value, so a fix that got the sign wrong (ZExt where §5.1
// wants SExt for a signed source) would be caught: `int` → `long` must
// sign-extend.
BEZEL_TEST(dict_widened_key_6541,
           dict_negative_widened_key_sign_extends_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out(prog("var i := -7; " +
                 fiveMembers("Dictionary<long, string>", "-7L", "i"))),
        kAllFound);
}

// An UNSIGNED source widening (`uint` → `long`), the opposite ZExt/SExt case.
BEZEL_TEST(dict_widened_key_6541,
           dict_unsigned_widened_key_zero_extends_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out(prog("var u := 7U; " +
                 fiveMembers("Dictionary<long, string>", "7L", "u"))),
        kAllFound);
}

// The ABSENT widened key: `ContainsKey(8)` must still answer false. A fix that
// widened by truncating to the source width would make every key collide.
BEZEL_TEST(dict_widened_key_6541,
           dict_widened_key_absent_control_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var d := new Dictionary<long, string>(); "
            "d.Add(7L, \"K\"); "
            "var i := 8; "
            "Console.WriteLine($\"C={d.ContainsKey(i).ToString()} \"); "
            "var g := d.GetOrNull(i); "
            "Console.WriteLine($\"G={(if g = null then \"null\" else g!)} \"); "
            "var q := i in d; "
            "Console.WriteLine($\"I={q.ToString()}\"); ")),
        "C=False \nG=null \nI=False\n");
}

// A large `int` widened to `long`: `2_000_000_000` does not fit in a `byte`
// or a `short`, so a Trunc-everywhere fix would collide it with a small key.
BEZEL_TEST(dict_widened_key_6541,
           dict_widened_large_int_key_spec_5_1) {
    BEZEL_EXPECT_EQ(
        out(prog("var i := 2000000000; " +
                 fiveMembers("Dictionary<long, string>", "2000000000L",
                             "i"))),
        kAllFound);
}

// A generic RECORD key — the `keyIsRecord` arm spills an existing address and
// must be untouched by a numeric coercion.
BEZEL_TEST(dict_widened_key_6541, dict_record_key_control_spec_8_3) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "record P(X: int); "
            "var d := new Dictionary<P, string>(); "
            "d.Add(new P(3), \"K\"); "
            "Console.WriteLine($\"C={d.ContainsKey(new P(3)).ToString()} \"); "
            "var g := d.GetOrNull(new P(3)); "
            "Console.WriteLine($\"G={(if g = null then \"null\" else g!)}\"); ")),
        "C=True \nG=K\n");
}

// ── The sanitizer rows: the pre-fix failure is a stack-buffer-overflow, so
//    "the values happen to match" is not evidence — the memory error must be
//    gone. A plain-build run cannot see it (the over-read returns whatever the
//    stack held), so these carry the load.
BEZEL_TEST(dict_widened_key_6541, dict_widened_key_asan_clean_spec_5_1) {
    BEZEL_EXPECT_TRUE(sanitizerClean(prog(
        "var d := new Dictionary<long, string>(); "
        "d.Add(7L, \"K\"); "
        "var i := 7; "
        "var a := d.GetOrNull(i); "
        "var b := d.ContainsKey(i); "
        "var c := i in d; "
        "var e := d[i]; "
        "var f := d.Remove(i); "
        "Console.WriteLine($\"{a!}{b.ToString()}{c.ToString()}{e!}"
        "{f.ToString()}\"); ")));
}

// Loop-amplified across the widening, so a per-call miscompile cannot hide in
// a single lookup's luck.
BEZEL_TEST(dict_widened_key_6541,
           dict_widened_key_asan_clean_loop_spec_5_1) {
    BEZEL_EXPECT_TRUE(sanitizerClean(prog(
        "var d := new Dictionary<long, int>(); "
        "var n := 0; "
        "while n < 200 do "
        "  d.Add(n, n); "
        "  var i := n; "
        "  if not d.ContainsKey(i) then return 2; end; "
        "  var v := d.GetOrNull(i); "
        "  if v = null then return 3; end; "
        "  if v! <> n then return 4; end; "
        "  var q := i in d; "
        "  if not q then return 5; end; "
        "  n := n + 1; "
        "end while; "
        "Console.WriteLine($\"{d.Count.ToString()}\"); ")));
}

// ── The negative control for §5.1's NEVER-IMPLICIT list: a non-constant
//    narrowing at a key slot is a compile error and must STAY one. #6527 fixed
//    the rejection side of this gate; this row pins that the widening fix did
//    not quietly make the check permissive. As above, the assertion is the
//    strong one: a real rejection at a real location carrying the §5.1 wording,
//    so a crash or an ASan report cannot satisfy it (issue #3892).
BEZEL_TEST(dict_widened_key_6541,
           dict_non_constant_narrowed_key_still_rejected_spec_5_1) {
    auto r = runAot(prog(
        "var d := new Dictionary<byte, string>(); "
        "d.Add(5, \"K\"); "
        "var i := 5; "
        "Console.WriteLine($\"{d.ContainsKey(i).ToString()}\"); "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument to `Dictionary<T>.ContainsKey` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
}