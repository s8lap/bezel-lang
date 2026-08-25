// fix #2678 — sema admits `Dictionary<Array<int>, string>` when a NESTED
// collection literal appears in KEY position; codegen then ICEs at
// `materializeHeapDictLiteral`'s `lowerExpr(*cl->keys[i])` site with
// `internal: lowerExpr returned null for dictionary literal key or value — #1386`.
//
// §28.11 ("Collection element ownership", v0.282) and stdlib §4.8 ("Dictionary")
// state that a dictionary's KEY slots are value-like (the borrowing column):
//   - the requirement on `K` is the §3.6 hashable derived-property;
//   - "deferred to instantiation" admits ANY concrete key shape at the type-
//     parameter level (the #1384 generic-param pass),
//   - but a **collection literal** at the KEY position is degenerate at run time:
//     the literal mints a FRESH node (per §15.4's literal-tree rule), the
//     dictionary would then key by reference identity of that fresh node, and
//     the runtime cannot make this well-defined (§4.8's equality contract
//     expects content equality; a freshly-minted collection is mutable and
//     changes after insertion would silently desynchronize the key from what
//     was inserted).
//
// #2678's fix shape is therefore narrows-the-source: a NESTED `CollectionLiteralExpr`
// in a dictionary-literal KEY position is rejected at the literal site, with
// a clear "non-hashable key type" message that does not ICE codegen. The fix
// is LITERAL-ONLY (mirroring §15.4's recorded resolution 5 — Add after
// construction is not a literal position), so:
//   - `d.Add([1, 2], "x")` stays admissible (a §15.4 literal-tree node with
//     no hashing burden — the literal is freshly minted at the Add site and
//     its lifetime is the call, not the entry);
//   - `var d: Dictionary<Array<int>, string> := ...; d.Add(new Array<int>(2), "a");`
//     stays admissible too (no literal in key position);
//   - `var d := { "k": [1, 2] };` (literal in VALUE position) stays admissible
//     — the literal-tree rule + §15.4's fresh-node admission still apply.
//
// The rejection lives at the SAME site as #2673 / #3307
// (`checkBorrowingFamilyLiteralElements`), but the predicate is orthogonal:
// #2673/#3307 fire on "fresh producer of non-trivial-dispose class type" —
// `Array<int>` is borrowing, not non-trivial, so neither arm fires. This
// file's predicate fires on "nested collection literal in KEY position",
// regardless of the literal's element category.
//
// SPEC CITATIONS:
//   - §28.11 — "Collection element ownership": the per-position classification
//     table (key slots are value-like);
//   - §15.4 — "Collection literals": the literal-tree rule (the semantics
//     that make a literal-in-key degenerate).
// (issue #2678)
//
// fail-without / pass-with: on `4387b45` every BEZEL_EXPECT_FALSE(r.ran) row
// below fails — `lowerExpr` returns null on the nested key and the program ICEs
// before reaching the runtime; with the fix each reports the §28.11 diagnostic
// and `r.ran` is false. Accept-controls pass identically before and after.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Module prelude for every program below.
const char* kPrelude =
    "module M; ";

// Wrap a `Main` body in the prelude + a host class.
std::string prog(const std::string& body) {
    return std::string(kPrelude) +
           "class C "
           "  public static method Main(): int " +
           body +
           "  end Main; "
           "end C;";
}

// True iff SOME diagnostic carries the §28.11 key-position rejection. The
// match is on the RULE's own language plus the §-citation, so that a
// different diagnostic (the §15.4 borrowing-family fresh-producer rejection
// on the value position, a §3.6 hashable-property rejection on the type
// annotation, an internal-error ICE) cannot satisfy it.
bool rejectedKeyCollectionLiteral(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("key") != std::string::npos &&
            d.message.find("collection literal") != std::string::npos &&
            d.message.find("\xC2\xA7""28.11") != std::string::npos)
            return true;
    return false;
}

// True iff the run produced an internal-error / ICE diagnostic — the
// pre-#2678 failure mode. A pass-with-out fix is meaningless if it ICEs in
// the same place; this guard pins that.
bool hasInternalErrorDiagnostic(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("internal:") != std::string::npos ||
            d.message.find("#1386") != std::string::npos)
            return true;
    return false;
}

// Assert one reject row: the program does not run, it didn't ICE, and it
// failed for THIS rule.
void expectRejected(const std::string& body) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_FALSE(hasInternalErrorDiagnostic(r));
    BEZEL_EXPECT_TRUE(rejectedKeyCollectionLiteral(r));
}

// Assert one accept row BY VALUE: it compiles, runs, and computes `expected`.
void expectAccepted(const std::string& body, int expected) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, expected);
}

}  // namespace

// =====================================================================
// REJECT — the issue's repro and the four adjacent dictionary-key shapes
// =====================================================================

// The issue's cardinal repro, verbatim from #2678: an array literal in the
// KEY position of a Dictionary literal whose K is the corresponding heap-
// collection type. Pre-fix: ICEs at `lowerExpr returned null for dictionary
// literal key or value — #1386`. Post-fix: rejected with the §28.11
// diagnostic naming the key position. // per §28.11, §15.4 (#2678)
BEZEL_TEST(sema_dict_key_collection_literal_rejected_2678,
           array_literal_in_dictionary_key_position_rejected_spec_28_11) {
    expectRejected(
        "    var d: Dictionary<Array<int>, string> := [[1, 2]: \"a\"]; "
        "    return 0; ");
}

// The same shape with `Dictionary<Array<string>, int>` — exercises the
// ARRAY-LITERAL handler again with a different element type, to prove the
// check is keyed on the literal-in-key position and not on the element
// primitive type. // per §28.11 (#2678)
BEZEL_TEST(sema_dict_key_collection_literal_rejected_2678,
           array_literal_string_element_in_dict_key_rejected_spec_28_11) {
    expectRejected(
        "    var d: Dictionary<Array<string>, int> := [[\"x\", \"y\"]: 7]; "
        "    return 0; ");
}

// The dict-form target-inferred variant: no explicit annotation, the outer
// literal's target is inferred from its value type. `string` is the value
// type; the key type is inferred from the inner literal `[1, 2]`, which
// becomes `Array<int>` (§15.4's default sequence element target). The bug
// is the same — a literal in key position produces an ICE in the codegen
// materializer — and the fix catches it at the same site.
// per §28.11 (#2678)
BEZEL_TEST(sema_dict_key_collection_literal_rejected_2678,
           nested_array_literal_target_inferred_dict_key_rejected_spec_28_11) {
    expectRejected(
        "    var d := [[1, 2]: \"a\"]; "
        "    return 0; ");
}

// =====================================================================
// ACCEPT-CONTROLS — pin the neighbours the fix must NOT widen to.
// Each pinned by VALUE.
// =====================================================================

// THE cardinal accept-control: a collection literal at the dict VALUE
// position (§15.4's literal-tree rule applies; the value's hashing
// contract doesn't gate on it the way the key's does). The fresh `[1, 2]`
// in value position MUST stay admissible with `d.Count` == 1 — exactly
// what `Dictionary<string, Array<int>> := ["primes": [2,3,5,7]]` was after
// #2660 added the lower-materialization for nested VALUE literals.
// per §15.4, §28.11 (#2678)
BEZEL_TEST(sema_dict_key_collection_literal_rejected_2678,
           array_literal_in_dictionary_value_position_admitted_spec_15_4) {
    expectAccepted(
        "    var d: Dictionary<string, Array<int>> := [\"p\": [2, 3, 5, 7]]; "
        "    return d.Count; ",
        1);
}

// The dict where the KEY is a primitive (no fresh collection at the key
// position) and the VALUE is a nested literal — a neighbour the fix must
// not touch, and the cardinality is pinned at 2 to prove both entries
// landed. // per §15.4, §28.11 (#2678)
BEZEL_TEST(sema_dict_key_collection_literal_rejected_2678,
           primitives_in_keys_array_in_value_admitted_spec_15_4) {
    expectAccepted(
        "    var d: Dictionary<int, Array<int>> := [1: [10, 11], 2: [20, 21]]; "
        "    return d.Count; ",
        2);
}

// The trivial-dispose nested literal in the SEQUENCE position — the
// shape that #2673's `nested_trivial_dispose_literal_admitted_spec_15_4`
// already pins for `Array<Array<int>> := [[1, 2], [3]]`. Length = 2.
// The fix is LITERAL-ONLY at the KEY position; this neighbour stays
// admissible. // per §15.4 (#2678)
BEZEL_TEST(sema_dict_key_collection_literal_rejected_2678,
           nested_array_literal_in_sequence_position_admitted_spec_15_4) {
    expectAccepted(
        "    var g: Array<Array<int>> := [[1, 2], [3, 4]]; "
        "    return g.Length; ",
        2);
}

// A NON-literal key of the same degenerate shape would be admissible
// (this is `Dictionary<Array<int>, string>` with a key that's an
// identifier-typed `using var` binding of `Array<int>`); it just keys
// by reference identity per the §3.6 rule that `Array<int>` (a class)
// IS hashable on reference identity. The bug is specifically the literal
// in key position — a fresh mutable collection whose runtime hash
// contract is undefined. This control proves the fix does not widen to
// reject the type. Length = 1. // per §3.6, §28.11 (#2678)
BEZEL_TEST(sema_dict_key_collection_literal_rejected_2678,
           named_array_key_value_dict_admitted_spec_3_6_28_11) {
    expectAccepted(
        "    using var k := new Array<int>(); "
        "    k.Add(1); "
        "    var d: Dictionary<Array<int>, string> := [k: \"a\"]; "
        "    return d.Count; ",
        1);
}
