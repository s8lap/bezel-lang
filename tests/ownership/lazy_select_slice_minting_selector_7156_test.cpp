// Ownership/§28.3 + §4.6 + stdlib §9.6 (#7156) — §28.3's lazy-`Select`
// MINTING-SELECTOR gate could not see the SLICE spelling, because it
// recognised a minting selector only through a member CALL's NAME.
//
// ROOT CAUSE (src/ownership/Ownership_Checks.cpp). The §28.3 lazy-`Select`
// gate rejects a selector whose body MINTS a fresh owned value per element:
// stdlib §5 classifies `Select`'s projected element as a RECEIVER-SOURCED
// BORROW, so the lazy pipeline takes no disposal obligation on the projection
// and every minted projection leaks. The gate recognised a string-minting
// selector through a NAME list of the stdlib §9.6 members whose contract is
// "returns a new `string` value; original unchanged" (the list #1931
// introduced; #7144 completed it against stdlib §9.6 v0.343).
//
// stdlib §9.6 (v0.343) also declares the SLICE as a PROPERTY —
//
//   public pure readonly property [r: Range<int>]: string;
//
// — and language spec §4.6 (v0.343) says of it: "`s[r]` for a `Range<int>`
// value `r` returns the substring spanning `r` [...] The result is a fresh
// `string`". A PROPERTY is not a member CALL, so the gate's shape never saw
// it: `Select(s => s[0..2])` was ACCEPTED.
//
// CONSEQUENCE, MEASURED on the base tree (both arms):
//   * the in-range slice LEAKED — 72 B in 4 allocations for two elements (64 B
//     direct + 8 B indirect, the fresh slice string's header and its bytes);
//   * an OUT-OF-RANGE slice SEGFAULTED — ASan: "SEGV on unknown address
//     0x000000000008 ... caused by a READ memory access", a null-page read.
//     §4.6 says an out-of-range endpoint raises `IndexOutOfRangeException`, and
//     the SAME slice OUTSIDE a selector does raise it (exit 70) — so the crash
//     is reachable only through the accepted selector shape.
//
// FIX. The gate now asks the RESULT-SHAPE question — "is this expression's
// result a fresh `string`?" — over every fresh-string shape at once:
// concatenation, interpolation, the §9.6 slice PROPERTY, and the §9.6 minting
// member set. The slice is recognised by the SHAPE of its index expression
// (a `Range` index on a `string` receiver), NOT by a name, so the next
// §9.6 member — or the next §9.6 PROPERTY — cannot slip past it the way
// `PadCenter` did.
//
// THE TWO GATES AGREE. The slice selector is rejected by the SAME §28.3 gate
// that rejects `PadCenter`, with the SAME diagnostic text: the row
// `select_string_slice_diagnostic_matches_padcenter_spec_28_3` asserts the two
// programs' error messages are EQUAL, not merely similar.
//
// OVER-REJECTION GUARD (both directions measured). The gate must keep admitting
// a selector that returns a BORROWED string, and it does: a bare element read,
// a field read of a `string`, an index read of a `string` element, a
// user-defined method returning a borrowed string, and §9.6's `ToString()`
// (the one instance member whose result is a `string` but which does not mint)
// all stay ADMITTED and run leak-free under the LSan-armed oracle. The slice
// OUTSIDE a selector stays ADMITTED and leak-free too — the gate is a SELECTOR
// gate, not a slice gate.
//
// Spec: language spec §28.3 *Bare-type-parameter returns* / *Unbound owned
// temporaries* (v0.194 — laundering a fresh owned value out through a borrow
// result), §4.6 `string` (v0.343 — the slice's "the result is a fresh
// `string`"), stdlib §5 "Ownership at class-typed substitutions" (v0.194 — the
// projected element is a receiver-sourced borrow), stdlib §9.6 `String`
// (v0.343 — the `[r: Range<int>]` property). Spec revision v0.343.

#include <string>
#include <string_view>

#include "support/aot_support.hpp"   // runAot / runAotChecked — the AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The §28.3 minting-selector diagnostic TEXT, extracted from the harness's
// captured compiler stderr.
//
// WHY THE EXTRACTION. The AOT harness reports a compile REJECT as ONE Error
// diagnostic whose message is the WHOLE captured stderr, and that stderr's
// first line may be a WARNING carrying the temp program's own path — which
// differs from run to run. So the text is compared FROM the diagnostic's
// marker onwards: two programs that produce the SAME diagnostic then produce
// the SAME string, and a second wording for one shape shows up as a
// difference.
std::string mintingDiagnosticText(const std::string& src) {
    const std::string marker = "a lazy `Select` selector that mints";
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        auto p = d.message.find(marker);
        if (p == std::string::npos) return "";
        return d.message.substr(p);
    }
    return "";
}

// The specific §28.3 minting-SELECT diagnostic (distinguishes the intended
// reject from any incidental error).
bool saysMintingSelect(const std::string& src) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("`Select` selector that mints") != std::string::npos)
            return true;
    }
    return false;
}

// The §28.3 minting-selector diagnostic for the `Min` / `Max` aggregation
// family (the sibling gate #1915 opened — its own wording).
bool saysMintingMinMax(const std::string& src) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("`Min`/`Max` selector that mints") != std::string::npos)
            return true;
    }
    return false;
}

// runAotChecked compiles + links + runs under generated-code ASan/LSan; a leak,
// double-free, or crash flips r.ran to false -> -999 (never a legal exit value).
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// The `Select`/`Min`/`Max` selector programs, assembled from their pieces so
// the two gates' diagnostics can be compared textually.
std::string selectSlice(const std::string& body) {
    return "module M; import System.Collections.Linq; "
           "class C public static method Main(): int "
           "  using var words: Array<string> := [\"abc\", \"defg\"]; "
           "  var total: int := 0; "
           "  for var w in words.Select(lambda(s) => " + body + ") do "
           "    total := total + w.Length; "
           "  end for; "
           "  return total; "
           "end Main; end C;";
}

}  // namespace

// ===========================================================================
// REJECT — the SLICE minting selectors (§28.3 / §4.6 / stdlib §9.6, #7156).
// Fail-without: before the fix each of these compiled CLEAN and either LEAKED
// (72 B in 4 allocations for two elements) or SEGFAULTED.
// ===========================================================================

// language spec §4.6 (v0.343) / §28.3 (#7156): the issue's canonical repro —
// `Select(s => s[0..2])` is the §9.6 `[r: Range<int>]` PROPERTY, whose §4.6
// contract is "the result is a fresh `string`". It is a mint, and the §28.3
// minting-selector gate rejects it.
BEZEL_TEST(lazy_select_slice_minting_selector_7156,
           select_string_slice_rejected_spec_28_3) {
    BEZEL_EXPECT_TRUE(saysMintingSelect(selectSlice("s[0..2]")));
}

// language spec §4.6 (v0.343) / stdlib §9.6 (v0.343) / §28.3 (#7156): the
// SECOND slice spelling — a `Range`-typed VALUE rather than a range literal
// (`s[r]`, the form #7119 gave its own lowering) — is the same §9.6 property
// and the same mint. REJECTED.
BEZEL_TEST(lazy_select_slice_minting_selector_7156,
           select_string_slice_range_value_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"abc\", \"defg\"]; "
        "  const r: Range<int> := 0..2; "
        "  var total: int := 0; "
        "  for var w in words.Select(lambda(s) => s[r]) do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingSelect(src));
}

// language spec §4.6 (v0.343) / stdlib §5 / §28.3 (#7156): `Min` shares the
// §28.3 minting-selector gate with `Select` (the same gate #1915 opened for
// `Min` / `Max`), so `Min(lambda(s) => s[0..2])` is REJECTED too.
BEZEL_TEST(lazy_select_slice_minting_selector_7156,
           min_string_slice_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"abc\", \"defg\"]; "
        "  var best: string := words.Min(lambda(s) => s[0..2]); "
        "  return best.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingMinMax(src));
}

// language spec §4.6 (v0.343) / stdlib §5 / §28.3 (#7156): `Max` is the
// sibling aggregation gate — the same slice is REJECTED there too.
BEZEL_TEST(lazy_select_slice_minting_selector_7156,
           max_string_slice_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"abc\", \"defg\"]; "
        "  var best: string := words.Max(lambda(s) => s[0..2]); "
        "  return best.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingMinMax(src));
}

// language spec §4.6 (v0.343) / §28.3 (#7156): the reject is POSITION-
// INDEPENDENT — a `.ToArray()` materialization of a slice selector mints
// identically. REJECTED at the selector.
BEZEL_TEST(lazy_select_slice_minting_selector_7156,
           select_string_slice_toarray_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"abc\", \"defg\"]; "
        "  using var arr: Array<string> := "
        "    words.Select(lambda(s) => s[0..2]).ToArray(); "
        "  return arr.Count; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingSelect(src));
}

// language spec §4.6 (v0.343) / §28.3 (#7156): THE CRASH REPRO. The element
// is shorter than the slice's upper endpoint, so §4.6's slice raises
// `IndexOutOfRangeException`. MEASURED on the base tree: inside a `Select`
// selector that raise did NOT propagate — the projection yielded a null and
// reading a member of it SEGFAULTED (ASan: "SEGV on unknown address
// 0x000000000008 ... caused by a READ memory access"). With the slice rejected
// at compile time the program never runs, so the crash is unreachable through
// this spelling.
BEZEL_TEST(lazy_select_slice_minting_selector_7156,
           select_string_slice_out_of_range_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"a\", \"bb\"]; "
        "  var total: int := 0; "
        "  for var w in words.Select(lambda(s) => s[0..2]) do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingSelect(src));
}

// language spec §28.3 / stdlib §9.6 (v0.343) (#7156): THE TWO GATES AGREE.
// The slice selector's diagnostic is not merely "a §28.3 minting-selector
// error" — it is the SAME TEXT the #7144 gate emits for its `PadCenter`
// selector. This row compares the two messages for EQUALITY, so a second
// wording can never be introduced for one shape without the other.
BEZEL_TEST(lazy_select_slice_minting_selector_7156,
           select_string_slice_diagnostic_matches_padcenter_spec_28_3) {
    std::string sliceMsg = mintingDiagnosticText(selectSlice("s[0..2]"));
    std::string padMsg = mintingDiagnosticText(selectSlice("s.PadCenter(5)"));
    BEZEL_EXPECT_TRUE(!sliceMsg.empty());
    BEZEL_EXPECT_TRUE(!padMsg.empty());
    BEZEL_EXPECT_TRUE(sliceMsg == padMsg);
}

// ===========================================================================
// ACCEPT (over-rejection guard) — BORROWED selectors must stay admitted and
// run leak-free, and the SLICE ITSELF must keep working outside a selector.
// ===========================================================================

// stdlib §5 (v0.194) / §28.3 (#7156): the borrowing selector `Select(s => s)`
// (the element itself) is NOT a mint — it stays admitted and leak-free.
// 3 + 4 = 7.
BEZEL_TEST(lazy_select_slice_minting_selector_7156,
           select_string_identity_accepted_leakfree_spec_5) {
    BEZEL_EXPECT_EQ(run(selectSlice("s")), 7);
}

// stdlib §5 (v0.194) / §28.3 (#7156): an INDEX read of a `string` element
// (`words[i]`) is a BORROW of the element, not a mint — admitted, leak-free.
// 1 + 2 = 3.
BEZEL_TEST(lazy_select_slice_minting_selector_7156,
           select_string_index_read_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"a\", \"bb\"]; "
        "  using var idx: Array<int> := [0, 1]; "
        "  var total: int := 0; "
        "  for var w in idx.Select(lambda(i) => words[i]) do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 3);
}

// stdlib §5 (v0.194) / §28.3 (#7156): a FIELD read of a `string` is a BORROW
// of the receiver's slot — admitted, leak-free. "a" + "bb" = 3.
BEZEL_TEST(lazy_select_slice_minting_selector_7156,
           select_string_field_read_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "record R(Name: string); "
        "class C public static method Main(): int "
        "  using var rows: Array<R> := [new R(\"a\"), new R(\"bb\")]; "
        "  var total: int := 0; "
        "  for var w in rows.Select(lambda(r) => r.Name) do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 3);
}

// stdlib §5 (v0.194) / §28.3 (#7156): a USER-DEFINED method returning a
// BORROWED string is left alone by the gate (the no-`MethodDecl` gate in the
// §9.6 mint-set test) — admitted, leak-free. 1 + 2 = 3.
BEZEL_TEST(lazy_select_slice_minting_selector_7156,
           select_user_method_borrowed_string_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C "
        "  private static method Identity(s: string): string => s; "
        "  public static method Main(): int "
        "    using var words: Array<string> := [\"a\", \"bb\"]; "
        "    var total: int := 0; "
        "    for var w in words.Select(lambda(s) => Identity(s)) do "
        "      total := total + w.Length; "
        "    end for; "
        "    return total; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 3);
}

// stdlib §9.6 (v0.343) / §28.3 (#7156): `ToString()` is §9.6's ONE instance
// member whose result is a `string` but which does NOT mint — §9.6 says it
// "returns this". A `ToString` selector stays ADMITTED and leak-free.
// 3 + 4 = 7.
BEZEL_TEST(lazy_select_slice_minting_selector_7156,
           select_string_tostring_accepted_leakfree_spec_9_6) {
    BEZEL_EXPECT_EQ(run(selectSlice("s.ToString()")), 7);
}

// language spec §4.6 (v0.343) (#7156): the SLICE ITSELF keeps working OUTSIDE
// a selector — the gate is a SELECTOR gate, not a slice gate. The literal
// slice `s[0..2]` of "abcdef" is "abc" — length 3 — and the program is
// leak-free under the LSan-armed oracle.
BEZEL_TEST(lazy_select_slice_minting_selector_7156,
           literal_slice_outside_selector_accepted_leakfree_spec_4_6) {
    std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  const s: string := \"abcdef\"; "
        "  const t: string := s[0..2]; "
        "  return t.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 3);
}

// language spec §4.6 (v0.343) (#7156): the out-of-range slice OUTSIDE a
// selector still RAISES §4.6's `IndexOutOfRangeException` (the runtime's
// unhandled-exception exit code, 70) — the compile-time rejection of the
// SELECTOR spelling does not change the slice's own semantics.
BEZEL_TEST(lazy_select_slice_minting_selector_7156,
           literal_slice_out_of_range_raises_spec_4_6) {
    std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  const s: string := \"a\"; "
        "  const t: string := s[0..2]; "
        "  return t.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 70);
}

// language spec §28.3 / stdlib §9.6 (v0.343) (#7144, #7156): the SIBLING gate
// is untouched — `PadCenter` (and `PadLeft`) are still rejected by the same
// §28.3 minting-selector diagnostic.
BEZEL_TEST(lazy_select_slice_minting_selector_7156,
           select_string_padcenter_still_rejected_spec_28_3) {
    BEZEL_EXPECT_TRUE(saysMintingSelect(selectSlice("s.PadCenter(5)")));
}

// language spec §28.3 / stdlib §9.6 (v0.343) (#7144, #7156): `PadLeft` — the
// sibling #7144 did not have a row for — is still rejected by the same gate.
BEZEL_TEST(lazy_select_slice_minting_selector_7156,
           select_string_padleft_still_rejected_spec_28_3) {
    BEZEL_EXPECT_TRUE(saysMintingSelect(selectSlice("s.PadLeft(5)")));
}

// language spec §28.3 / §4.6 (v0.343) (#7156): the CONCATENATION arm of the
// result-shape predicate is untouched by this change — a `+` selector is still
// rejected.
BEZEL_TEST(lazy_select_slice_minting_selector_7156,
           select_string_concat_still_rejected_spec_28_3) {
    BEZEL_EXPECT_TRUE(saysMintingSelect(selectSlice("s + \"x\"")));
}

// language spec §28.3 / §4.6 (v0.343) (#7156): the INTERPOLATION arm of the
// result-shape predicate is untouched — an interpolated selector is still
// rejected.
BEZEL_TEST(lazy_select_slice_minting_selector_7156,
           select_string_interpolation_still_rejected_spec_28_3) {
    BEZEL_EXPECT_TRUE(saysMintingSelect(selectSlice("$\"{s}\"")));
}
