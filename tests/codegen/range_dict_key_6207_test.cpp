// Regression tests for issue #6207: `Dictionary<Range<int>, V>` compiled with no
// diagnostic and SIGSEGV'd (rc 139) on the first indexed store.
//
// stdlib §9.8 (v0.338): "`Range<T>` is a compiler-intrinsic record" whose "All six
// declared properties are the type's auto-property surface for the language's
// generated members (spec §8.5/§9.2)". Spec §8.3 (v0.338) gives a BODY-ONLY
// RECORD "property-by-property value equality" and a hash of the same, with
// "if `a.Equals(b)` then `a.GetHashCode() = b.GetHashCode()`" and a combining
// function that "**must be order-sensitive**" (plain XOR does not satisfy it).
// §8.3 admits every type except "(a) a record that is neither auto-hashable nor
// declares user equality, and (b) a choice that is not value-like
// auto-hashable" — a range is neither — so it is an ADMISSIBLE `Dictionary` key
// and the store must work.
//
// MEASURED on the pre-fix base: `Dictionary<Range<int>, int>` compiled clean and
// died with `Segmentation fault (core dumped)`, rc 139, for both the `Dictionary`
// and the `OwningDictionary` spelling. Under ASan the fault was a READ in
// `bz_obj_is_string` called from `bz$valkey$hash` <- `bz_dict_set_gen`; the
// faulting ADDRESS tracked the range's `Start` exactly (`1..5` -> `0x1`,
// `7..9` -> `0x7`, `3..4` -> `0x3`), i.e. a fully-initialised integer
// dereferenced as an object pointer.
//
// Cause: `SemaContext::builtinClassSym` stamps `Range` with `kind = Class` (and
// `isValueBuiltin = true`) so the member-access paths keep working. A `Range<T>`
// TypeRef KEEPS that declaration (ExprTypecheck_Exprs.cpp and
// TypeResolution_Names.cpp both re-attach `builtinClassSym("Range")`, where most
// other value builtins resolve with `declaration = nullptr`), so
// `hasObjectHeaderEquality` — which tests only `kind == Class || kind ==
// Interface` — answered "carries the §8.3 object header" and
// `buildValueHashTrampoline` / `buildValueEqTrampoline` dispatched the key
// through `lowerObjectGetHashCode` / `lowerObjectEquals`, reading word 0 of the
// `{i64,i64,i64,i8}` range storage as a vtable pointer. `Start` is that word.
//
// The fix gives the two trampolines a `Range<T>` arm that calls
// `emitRangeStorageHash` / `emitRangeStorageEquals` — the ONE definitions #4126
// introduced, already shared with the direct `r.Equals` / `r.GetHashCode()`
// members and the box vtable thunk — and places it BEFORE the
// `hasObjectHeaderEquality` dispatch. It deliberately does NOT narrow that
// predicate; the `set_of_range_...` rows below pin why.
//
// The rows pin three things: the store must not crash, the key's identity must be
// §8.3's STORAGE equality (an equal range replaces, a different one does not), and
// the key spellings must agree with the direct members. The `set_of_range_*` rows
// pin the SCOPE BOUNDARY — a `Set<Range<int>>` literal must keep REJECTING, because
// a `Set<T>` ELEMENT's identity runs through the array-backed dedup insert, not
// through the pair of trampolines this fix touches.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"  // runAot
#include "test_harness.hpp"

#include <regex>
#include <string>
#include <utility>
#include <vector>

using bezel::test::runAot;

namespace {

std::string prog(const std::string& mainBody, const std::string& decls = "") {
    return "module M;\n" + decls +
           "class C\n  public static method Main(): int\n" + mainBody +
           "  end Main;\nend C;\n";
}

// stdout of a program that must compile, run and exit 0. A row that did not run
// reads as a marker no expectation equals — so a CRASH fails this row rather
// than silently passing it.
std::string out(const std::string& mainBody, const std::string& decls = "") {
    auto r = runAot(prog(mainBody, decls));
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// ── Why `r.ran == false` is not an oracle (issue #6207 review, finding 1) ──
//
// A `BEZEL_EXPECT_EQ(false, r.ran)` row is satisfied by ANY rejection. The
// non-author review MEASURED three programs through this very harness that
// differ from a pinned program only in text and pass the same assertion: a
// SYNTAX error (`set_parseerr`, reported at 5:5) and outright GARBAGE
// (`set_garbage`, reported at 5:5 through 5:11). So the row as written could
// not tell "the materialiser rejected this element type" from "this program is
// not valid Bezel at all", and a typo introduced into the program under test
// would silently keep it green.
//
// The replacement discriminates on WHERE the diagnostic points, not on what it
// says. `renderedLocations` recovers the `<path>:<line>:<col>: ` prefixes the
// DRIVER prints (Driver.cpp:632/644/773/785) from the captured stderr, and the
// row pins the set-literal element's own span. That has the two properties the
// bare `r.ran` lacks:
//
//   RENAME-PROOF. A rename of the diagnostic's MESSAGE leaves every location
//   untouched, so the row survives one. Pinning the `#1070` string instead
//   would freeze a diagnostic the author had no stake in — the original
//   author's stated reason for the weak shape, and a real concern, but not a
//   reason to accept an unfalsifiable row.
//
//   FALSIFIABLE. An unrelated rejection reports somewhere ELSE, so it fails.
//   MEASURED on the base: the pinned rejection is at the set literal's own
//   element span; the parse error at 5:5, the garbage at 5:5-5:11, and an
//   unresolved identifier at a different COLUMN of the same line. `rejectedAt`
//   requires EVERY rendered location to be the expected one, so a stray second
//   diagnostic anywhere fails it too.
//
// Reject-only by construction (the `r.ran` early-out below), which is the
// shape `scripts/check_vacuous_tests.py` classifies as CLASS A: an Error and a
// successful compile are mutually exclusive, so the assertion cannot be a
// tautology given a companion `EXPECT_TRUE(r.ran)`.

// Every `<path>:<line>:<col>: ` location rendered in `r`'s diagnostics, as
// (line, column) pairs. A genuine `CompileOutcome::Rejected` packs the driver's
// ENTIRE stderr into ONE Diagnostic at `SourceLocation{}` (aot_support.hpp), so
// the locations live in the message TEXT — which is why this scans text rather
// than reading a structured `d.location`, and why `failedAtCompileTimeWithRealLocation`
// regex-scans for the same reason.
std::vector<std::pair<int, int>> renderedLocations(const bezel::test::JitResult& r) {
    static const std::regex kLoc(R"(:(\d+):(\d+): )");
    std::vector<std::pair<int, int>> locs;
    for (const auto& d : r.diagnostics.diagnostics()) {
        for (std::sregex_iterator it(d.message.begin(), d.message.end(), kLoc),
                                  end;
             it != end; ++it)
            locs.emplace_back(std::stoi((*it)[1].str()),
                              std::stoi((*it)[2].str()));
    }
    return locs;
}

// The (line, column) at which `marker` first appears in `src`, 1-based — the
// span the diagnostic under test points at. Derived from the program text
// rather than hardcoded, so reformatting the program cannot silently leave a
// stale expectation behind. Returns (-1, -1) when `marker` is absent, which
// makes `rejectedAt` false rather than vacuously true.
std::pair<int, int> spanOf(const std::string& src, const std::string& marker) {
    std::size_t at = src.find(marker);
    if (at == std::string::npos) return {-1, -1};
    int line = 1;
    for (std::size_t i = 0; i < at; ++i)
        if (src[i] == '\n') ++line;
    int col = 1;
    for (std::size_t i = at; i > 0 && src[i - 1] != '\n'; --i) ++col;
    return {line, col};
}

// True iff `r` is a genuine compile-time rejection whose EVERY rendered
// location is exactly `at`. Excludes the synthetic non-reject outcomes for the
// same reason `failedAtCompileTimeWithRealLocation` does: a Crashed or TimedOut
// diagnostic APPENDS whatever partial stderr the compiler emitted before dying,
// and that fragment can carry a real-looking location.
bool rejectedAt(const bezel::test::JitResult& r, std::pair<int, int> at) {
    if (r.ran) return false;  // a silent acceptance is the failure, not a pass
    for (const char* nonReject :
         {"compiler CRASHED", "terminated by signal", "did not finish within",
          "did not terminate within", "AddressSanitizer fired"})
        if (bezel::test::diagnosticMessageContains(r, nonReject)) return false;
    auto locs = renderedLocations(r);
    if (locs.empty()) return false;
    for (const auto& l : locs)
        if (l != at) return false;
    return true;
}

// `src` rejected, and every diagnostic it produced points at `marker`'s span in
// that same `src` — never at a parse error, a scope error, or any other site.
bool rejectedAtMarker(const std::string& src, const std::string& marker) {
    return rejectedAt(runAot(src), spanOf(src, marker));
}

}  // namespace

// ── The issue's own repro, verbatim ────────────────────────────────────────

BEZEL_TEST(range_dict_key_6207,
           issue_repro_dict_of_range_stores_and_reads_spec_8_3_stdlib_4_8) {
    // Pre-fix this SEGV'd in bz_obj_is_string on the first store. `out` returns
    // "<did not run>" when the binary cannot be run at all, so a crash fails
    // this row rather than passing it.
    BEZEL_EXPECT_EQ(std::string("7\n"),
                    out("    var d := new Dictionary<Range<int>, int>();\n"
                        "    d[1..5] := 7;\n"
                        "    Console.WriteLine(d[1..5]);\n"
                        "    return 0;\n"));
}

// The address the pre-fix SEGV read tracked the range's `Start`, so a non-1
// start pins the same defect from a different value — a fix that special-cased
// `0`/`1` would fail here.
BEZEL_TEST(range_dict_key_6207,
           nonzero_start_range_does_not_crash_spec_8_3_stdlib_9_8) {
    BEZEL_EXPECT_EQ(std::string("7\n"),
                    out("    var d := new Dictionary<Range<int>, int>();\n"
                        "    d[7..9] := 7;\n"
                        "    Console.WriteLine(d[7..9]);\n"
                        "    return 0;\n"));
}

// stdlib §4.9's OWNING sibling — a DIFFERENT store call site (the §4.9
// value-disposal path) lowering the same key type, so it is pinned separately.
BEZEL_TEST(range_dict_key_6207,
           owning_dict_of_range_key_stores_spec_8_3_stdlib_4_9) {
    BEZEL_EXPECT_EQ(std::string("7\n"),
                    out("    using var d := new OwningDictionary<Range<int>, "
                        "Box>();\n"
                        "    d[3..4] := new Box(7);\n"
                        "    Console.WriteLine(d[3..4].V);\n"
                        "    return 0;\n",
                        "class Box\n  public V: int;\n"
                        "  public constructor(v: int) V := v; end "
                        "constructor;\n"
                        "end Box;\n"));
}

// §8.3 "property-by-property value equality": an equal range is the SAME key, so
// the second store REPLACES rather than adds (stdlib §4.8: `d[key] := v` is
// insert-or-replace). A table keyed by the range's BYTES alone, or by anything
// but the full storage, would report two keys here.
BEZEL_TEST(range_dict_key_6207,
           equal_range_key_replaces_rather_than_duplicates_spec_8_3_stdlib_4_8) {
    BEZEL_EXPECT_EQ(std::string("9\n1\n"),
                    out("    var d := new Dictionary<Range<int>, int>();\n"
                        "    d[1..5] := 7;\n"
                        "    d[1..5] := 9;\n"
                        "    Console.WriteLine(d[1..5]);\n"
                        "    Console.WriteLine(d.Count);\n"
                        "    return 0;\n"));
}

// §8.3: each of the four stored words decides identity, so a range differing in
// any one of them is a DIFFERENT key and must not see the first range's value.
// `..<5` (IsEndInclusive) and `1..5 step 2` (Step) are the two the pair must not
// conflate with `1..5`. Read through `GetOrNull` (stdlib §4.8's `*OrNull` tier)
// with a `?? -1` floor, so a MISS prints `-1` and a wrong-but-present value is a
// different number: the row cannot pass by throwing, nor by missing the key for
// the wrong reason.
BEZEL_TEST(range_dict_key_6207,
           distinct_storage_is_a_distinct_key_spec_8_3_stdlib_4_8) {
    BEZEL_EXPECT_EQ(std::string("7\n-1\n-1\n-1\n"),
                    out("    var d := new Dictionary<Range<int>, int>();\n"
                        "    d[1..5] := 7;\n"
                        "    Console.WriteLine(d[1..5]);\n"
                        "    Console.WriteLine(d.GetOrNull(1..6) ?? -1);\n"
                        "    Console.WriteLine(d.GetOrNull(1..<5) ?? -1);\n"
                        "    Console.WriteLine(d.GetOrNull(1..5 step 2) ?? "
                        "-1);\n"
                        "    return 0;\n"));
}

// All THREE differing words as REAL stored keys, looked up by keys equal to
// them. This is the §8.3 order-sensitivity requirement observed as table
// behaviour rather than as a hash print: a commutative combiner would give
// `1..5` and `2..6` the same hash, and a table keyed on fewer than four storage
// words could not separate these three. Every stored key is found and no
// unstored key is.
BEZEL_TEST(range_dict_key_6207,
           three_stored_words_each_yield_their_own_entry_spec_8_3_stdlib_4_8) {
    BEZEL_EXPECT_EQ(std::string("3\n7\n8\n9\n"),
                    out("    var d := new Dictionary<Range<int>, int>();\n"
                        "    d[1..5] := 7;\n"
                        "    d[1..<5] := 8;\n"
                        "    d[1..5 step 2] := 9;\n"
                        "    Console.WriteLine(d.Count);\n"
                        "    Console.WriteLine(d[1..5]);\n"
                        "    Console.WriteLine(d[1..<5]);\n"
                        "    Console.WriteLine(d[1..5 step 2]);\n"
                        "    return 0;\n"));
}

// §16.3: "a `null` endpoint indicates the range is open-ended on that side", so
// `..5` is NOT the same key as `0..5` (or `1..5`) — the flags word differs even
// where a stored endpoint reads 0. This row reads the data the fix handles: a
// dictionary that conflated an open-ended range with a bounded one would print
// `1` where `2` is expected.
BEZEL_TEST(range_dict_key_6207,
           open_ended_range_is_a_distinct_key_spec_16_3_stdlib_9_8) {
    BEZEL_EXPECT_EQ(std::string("2\n7\n"),
                    out("    var d := new Dictionary<Range<int>, int>();\n"
                        "    d[..5] := 7;\n"
                        "    d[..<5] := 8;\n"
                        "    Console.WriteLine(d.Count);\n"
                        "    Console.WriteLine(d[..5]);\n"
                        "    return 0;\n"));
}

// §8.3's hash/equality law observed THROUGH the table: two ranges that are
// `Equals` must land in one bucket, so a lookup by the second finds the first's
// value. Keys bound to NAMED locals, so this exercises the key-spelling path
// (a stored `Range<T>` variable, not a literal) as well.
BEZEL_TEST(range_dict_key_6207,
           equal_ranges_share_one_table_entry_spec_8_3_stdlib_4_8) {
    BEZEL_EXPECT_EQ(std::string("7\n1\n"),
                    out("    var a: Range<int> := 1..5;\n"
                        "    var b: Range<int> := 1..5;\n"
                        "    var d := new Dictionary<Range<int>, int>();\n"
                        "    d[a] := 7;\n"
                        "    Console.WriteLine(d[b]);\n"
                        "    Console.WriteLine(d.Count);\n"
                        "    return 0;\n"));
}

// The key spellings must AGREE with the DIRECT members fix #4126 generates:
// §8.3's pair is one definition, so `r.Equals(o)` (the member) and the table's
// answer (the key path) may not drift. `a.Equals(b)` is True and the table finds
// `a`'s value under `b`; `a.Equals(c)` is False and the table misses.
BEZEL_TEST(range_dict_key_6207,
           key_path_agrees_with_direct_equals_spec_8_3_stdlib_9_8) {
    BEZEL_EXPECT_EQ(std::string("True\n7\nFalse\n-1\n1\n"),
                    out("    var a: Range<int> := 1..5;\n"
                        "    var b: Range<int> := 1..5;\n"
                        "    var c: Range<int> := 1..6;\n"
                        "    var d := new Dictionary<Range<int>, int>();\n"
                        "    d[a] := 7;\n"
                        "    Console.WriteLine(a.Equals(b));\n"
                        "    Console.WriteLine(d.GetOrNull(b) ?? -1);\n"
                        "    Console.WriteLine(a.Equals(c));\n"
                        "    Console.WriteLine(d.GetOrNull(c) ?? -1);\n"
                        "    Console.WriteLine(d.Count);\n"
                        "    return 0;\n"));
}

// The same §8.3 pair across the BOXED spelling (stdlib §8.6): `a.GetHashCode()`
// on the unboxed value must equal `GetHashCode()` on a `Range` boxed into a
// class field, which is what makes the trampoline's arm agree with the box
// vtable thunk #4126 built. Pre-fix the dictionary half was a SEGV, so this row
// is the cross-spelling consistency check the hash law names.
BEZEL_TEST(range_dict_key_6207,
           boxed_and_unboxed_spellings_agree_on_hash_spec_8_3_spec_8_6) {
    BEZEL_EXPECT_EQ(std::string("True\nTrue\n7\n"),
                    out("    var b := new Box(1..5);\n"
                        "    var d := new Dictionary<Range<int>, int>();\n"
                        "    d[1..5] := 7;\n"
                        "    Console.WriteLine(b.V.Equals(1..5));\n"
                        "    Console.WriteLine(b.V.GetHashCode() = "
                        "(1..5).GetHashCode());\n"
                        "    Console.WriteLine(d[1..5]);\n"
                        "    return 0;\n",
                        "class Box\n  public V: Range<int>;\n"
                        "  public constructor(v: Range<int>) V := v; end "
                        "constructor;\n"
                        "end Box;\n"));
}

// A `Range` VALUE (not key) in a dictionary, plus a `char`-element range: the
// fix is on the key path, so these pin that a range in the VALUE position — where
// no trampoline runs — is unchanged, and that the key arm is not `Range<int>`-
// specific.
BEZEL_TEST(range_dict_key_6207,
           range_in_value_position_and_char_element_key_are_unaffected_spec_8_3) {
    BEZEL_EXPECT_EQ(std::string("6\n7\n"),
                    out("    var d := new Dictionary<Range<int>, Range<int>>();\n"
                        "    d[1..5] := 2..6;\n"
                        "    Console.WriteLine($\"{d[1..5].End}\");\n"
                        "    var c := new Dictionary<Range<char>, int>();\n"
                        "    c[(char(97))..(char(122))] := 7;\n"
                        "    Console.WriteLine(c[(char(97))..(char(122))]);\n"
                        "    return 0;\n"));
}

// ── SCOPE BOUNDARY: `Set<Range<int>>` must keep REJECTING ─────────────────
//
// A `Set<T>` ELEMENT's identity runs through the ARRAY-BACKED dedup insert
// (`emitArrayBackedSetInsert` -> `emitRemoveElementEquality`, stdlib §4.10), a
// DIFFERENT surface from the pair of hash/equality trampolines this fix touches.
// The base compiler REJECTS a `Set<Range<int>>` LITERAL with a located
// diagnostic ("could not build element equality for a Set/OwningSet literal
// element", #1070). These rows pin that answer at the LITERAL spelling — the
// only spelling that reaches the set-literal materialiser at all.
//
// Why the literal and not `new Set<Range<int>>()` + `.Add(...)`: `Add` on the
// built-in `Set` is not implemented for `Set<T>` generally and rejects with
// "#2457", a diagnostic raised by `Add`'s OWN dispatch arm and so INDEPENDENT of
// the set-literal materialiser. A `new Set` + `.Add` row would therefore reject
// identically whether the materialiser guard worked or was irrelevant, and so
// cannot distinguish them. The literal is the only evidence available here.
//
// The RED arm is the point: these rows are red on a compiler that admits
// `Set<Range<int>>` as generic-backed and are green here and on the base.
//
// The rejection rows assert WHERE the diagnostic points, not merely THAT one
// was produced — see `rejectedAtMarker` for the measurement that forced this
// (`r.ran == false` alone is satisfied by a syntax error, by garbage, and by an
// unresolved identifier). The assertion is rename-proof: it pins the set
// literal's own source span, which a rename of the diagnostic's message does
// not move.

BEZEL_TEST(range_dict_key_6207,
           set_of_range_literal_still_rejects_spec_8_3_stdlib_4_10) {
    // `rejectedAtMarker` is the whole oracle — see the helper's comment for why
    // `r.ran == false` alone was not one. A silent-acceptance inversion here is
    // the defect class this row exists to catch: `r.ran` true with the program
    // compiled means a previously-rejected program began compiling, which is
    // the failure — not a pass — and `rejectedAt` returns false on it.
    BEZEL_EXPECT_TRUE(
        rejectedAtMarker(prog("    var s: Set<Range<int>> := [1..5, 7..9];\n"
                              "    Console.WriteLine(s.Count);\n"
                              "    return 0;\n"),
                         "1..5, 7..9"));
}

BEZEL_TEST(range_dict_key_6207,
           set_of_range_field_literal_still_rejects_spec_8_3_stdlib_4_10) {
    // The FIELD-initialised spelling reaches the same materialiser through a
    // different local-decl path (`lowerLocalDecl`'s #559 guard), so it is pinned
    // separately: a materialiser that returns null here is the shape that once
    // dropped a declaration with no diagnostic at all. The `Holder` declaration
    // is spliced in BEFORE `Main`, so the literal sits at line 3 — `spanOf`
    // derives that from the program rather than hardcoding it.
    BEZEL_EXPECT_TRUE(rejectedAtMarker(
        prog("    var h := new Holder();\n"
             "    Console.WriteLine(h.S.Count);\n"
             "    return 0;\n",
             "class Holder\n"
             "  public S: Set<Range<int>> := [1..5, 7..9];\n"
             "end Holder;\n"),
        "1..5, 7..9"));
}

BEZEL_TEST(range_dict_key_6207,
           set_of_range_as_a_dictionary_value_still_rejects_spec_8_3_stdlib_4_10) {
    // A `Set<Range<int>>` literal in a position whose OWN container is a
    // dictionary: the outer `Dictionary<int, …>` key path works (that is this
    // fix), and the inner `Set` literal must still be rejected — so this row
    // pins that the two surfaces are decided independently. Only the ONE
    // element is in the literal, so the span marker is the single range.
    BEZEL_EXPECT_TRUE(rejectedAtMarker(
        prog("    var d := new Dictionary<int, Set<Range<int>>>();\n"
             "    d[1] := [1..5];\n"
             "    Console.WriteLine(d[1].Count);\n"
             "    return 0;\n"),
        "1..5"));
}

BEZEL_TEST(range_dict_key_6207,
           empty_set_of_range_literal_still_compiles_and_reads_zero_spec_4_10) {
    // The adjacent ACCEPTED shape: an empty `Set<Range<int>>` literal carries no
    // range VALUE, so the element-equality failure the rows above pin cannot
    // arise, and it has always compiled. Pinning it keeps the rejection above
    // narrow — a fix that turned `Set<Range<int>> := []` into a rejection would
    // fail here, so the two rows together bracket exactly the boundary.
    BEZEL_EXPECT_EQ(std::string("0\n"),
                    out("    var s: Set<Range<int>> := [];\n"
                        "    Console.WriteLine(s.Count);\n"
                        "    return 0;\n"));
}

// ── Control rows: the neighbouring shapes must keep their answers ──────────

// A CLASS key still takes the §8.3 object path fix #2773 established. The
// dispatch this fix inserts a `Range` arm in front of is SHARED by every class
// and interface key, so this pins that the insertion removed only the `Range`
// mis-class and left the genuine reference keys object-dispatched: two
// DISTINCT `Box` instances are two keys here (reference identity), which a
// structural or value comparison would collapse to one.
BEZEL_TEST(range_dict_key_6207,
           control_class_key_still_dispatches_through_the_object_path_2773) {
    BEZEL_EXPECT_EQ(std::string("7\n9\n2\n"),
                    out("    var d := new Dictionary<Box, int>();\n"
                        "    var b := new Box(1);\n"
                        "    d[b] := 7;\n"
                        "    Console.WriteLine(d[b]);\n"
                        "    var c := new Box(1);\n"
                        "    d[c] := 9;\n"
                        "    Console.WriteLine(d[c]);\n"
                        "    Console.WriteLine(d.Count);\n"
                        "    return 0;\n",
                        "class Box\n  public V: int;\n"
                        "  public constructor(v: int) V := v; end "
                        "constructor;\n"
                        "end Box;\n"));
}

// A body-only RECORD key — the shape §8.3's auto-hashable row actually names —
// keeps its structural equality: equal records are one key, differing records
// are two. The trampoline's `recordTypeOfRef` arm precedes the fix's new arm.
BEZEL_TEST(range_dict_key_6207,
           control_record_key_still_compares_structurally_spec_8_3) {
    BEZEL_EXPECT_EQ(std::string("7\n-1\n1\n"),
                    out("    var d := new Dictionary<Point, int>();\n"
                        "    d[new Point(1, 5)] := 7;\n"
                        "    Console.WriteLine(d.GetOrNull(new Point(1, 5)) "
                        "?? -1);\n"
                        "    Console.WriteLine(d.GetOrNull(new Point(1, 6)) "
                        "?? -1);\n"
                        "    Console.WriteLine(d.Count);\n"
                        "    return 0;\n",
                        "record Point(X: int, Y: int);\n"));
}

// The OTHER value builtins keep their already-working answers. `Dictionary<Guid,
// int>` and `Dictionary<TimeSpan, int>` resolve with `declaration = nullptr` and
// so were NEVER claimed by `hasObjectHeaderEquality`; this row proves the fix
// changed nothing for them (it adds a `Range`-keyed arm only, and leaves that
// predicate alone). MEASURED on the base before the fix: both ran clean.
BEZEL_TEST(range_dict_key_6207,
           control_guid_key_unaffected_by_the_added_range_arm) {
    BEZEL_EXPECT_EQ(std::string("7\n-1\n"),
                    out("    var d := new Dictionary<Guid, int>();\n"
                        "    d[Guid.Empty] := 7;\n"
                        "    Console.WriteLine(d.GetOrNull(Guid.Empty) ?? -1);\n"
                        "    Console.WriteLine(d.GetOrNull(Guid.NewGuid()) "
                        "?? -1);\n"
                        "    return 0;\n"));
}

// `Dictionary<int, V>` — the scalar generic-key path the trampolines' fallback
// serves, and the same arm the fix's `Range` arm precedes.
BEZEL_TEST(range_dict_key_6207, control_scalar_key_unaffected_spec_4_8) {
    BEZEL_EXPECT_EQ(std::string("7\n"),
                    out("    var d := new Dictionary<int, int>();\n"
                        "    d[1] := 7;\n"
                        "    Console.WriteLine(d[1]);\n"
                        "    return 0;\n"));
}

// The sorted containers reject a `Range` element on stdlib §4.17/§4.18's
// `CompareTo` constraint (spec §14.2) — a different, EARLIER gate than the
// trampolines, and one this fix must not disturb. The span markers are the
// container names: this gate reports at the collection's own type argument, so
// pinning where it points also distinguishes it from the #1070 materialiser
// rejection the three rows above pin (a different site, and a different gate).
BEZEL_TEST(range_dict_key_6207,
           control_sorted_containers_still_reject_range_spec_14_2) {
    BEZEL_EXPECT_TRUE(
        rejectedAtMarker(prog("    var s := new SortedSet<Range<int>>();\n"
                              "    return 0;\n"),
                         "SortedSet<Range<int>>"));
    BEZEL_EXPECT_TRUE(
        rejectedAtMarker(prog("    var d := new SortedDictionary<Range<int>, "
                              "int>();\n"
                              "    return 0;\n"),
                         "SortedDictionary<Range<int>, int>"));
}