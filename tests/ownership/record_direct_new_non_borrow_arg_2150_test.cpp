// Spec-derived tests for issue #2150 — direct-`new` escape legs rooted ALL
// constructor arguments, but §9.7's borrow-lifetime rule scopes to the borrows
// actually STORED in borrow-carrying fields: "a borrow-bearing record value may
// not outlive the source of any borrow it CONTAINS." A value-typed positional
// field (int, enum, …) holds a VALUE, not a borrow, so rooting its argument
// via identifierRoot is wrong.
//
// §9.7 (Bezel_Language_Spec.md §9.7 "Records and disposal"):
//   "A borrow-bearing record value may not outlive the source of any borrow it
//    contains. For inline-array-field borrows, this applies element-wise …"
//   "A borrow-bearing record may not be returned from a method whose return-
//    type rules cannot establish a sufficient lifetime for every borrow the
//    record carries."
// The "borrow it contains" / "borrow the record carries" scope is exactly the
// borrow-carrying fields — a value stored in a value-typed positional field is
// not a borrow and cannot escape the frame through that field.
//
// Before the fix, checkRecordBorrowEscapesOnReturn's direct-`new` branch
// (legs (a) and (b) at Ownership_Checks.cpp ~2915/2928) iterated EVERY ctor
// argument and rooted each via identifierRoot, so `new Holder(p.AsSpan(),
// s.Count)` with `s` escape-bound spuriously rejected though only an int is
// stored in the int field. The fix pairs args with decl->positionalFields and
// skips non-borrow-carrying positions, exactly as isNullSourceRecordExpr
// (~2780) already does for the null-source ACCEPT recognizer.
//
// Each ACCEPT case here returns a distinct exit code so a silently wrong value
// is caught too. The escape-still-rooted GUARD (a real borrow via the span
// field is still rooted) is pinned by the lit cases
// tests/lit/ownership/record_direct_new_non_borrow_arg_2150__*. // per §9.7
// (v0.226, #2150)
//
// fix #4428 (§9.7, v0.301): the two ACCEPT cases below
// (span_field_plus_int_from_local_length_accepted,
//  int_from_local_length_plus_span_field_accepted) pinned the OPPOSITE of
// the v0.301 ruling — they asserted the parameter-rooted span leg (`p.AsSpan()`
// in the returned `new Holder(...)`) is admitted. The §9.7 v0.301 ruling
// closes the parameter arm: a parameter-sourced record return hands back a
// borrow no call site could bound. Renamed and re-polarized below to REJECT.
// The third ACCEPT case (generic_value_field_plus_span_accepted) is UNCHANGED:
// its span leg also uses `p.AsSpan()` on a parameter, so it now also REJECTS —
// re-polarized in place. Each now asserts the §9.7 v0.301 parameter diagnostic
// on the borrow-carrying field arg. // per §9.7, v0.301.

#include <string>

#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan/LSan oracle
#include "support/ownership_support.hpp"  // parseAndCheckOwnership, hasErrorMessage (#4428 §9.7 v0.301)
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

// ===========================================================================
// ACCEPT — precision improvement: a direct-`new` whose borrow-carrying field
// (the span) is sourced from a param (outlives the frame), and whose value
// field (int) is sourced from an escape-bound local via `s.Count`. The int
// field stores a VALUE (not a borrow), so rooting `s.Count` -> `s` is wrong;
// the return must be accepted.
// ===========================================================================

// §9.7 precision (#2150) / fix #4428 (§9.7 v0.301): the issue's exact repro —
// `return new Holder(p.AsSpan(), s.Count)` with `p` a parameter, `s` a
// scope-bound owned local — is REJECTED at the return site on the
// BORROW-CARRYING `S` field's argument (`p.AsSpan()`, rooted at the parameter
// `p`). The pre-#2150 path rooted `s.Count`->`s` (the value-typed `N` arg)
// via identifierRoot — that was the #2150 false-reject. The post-#2150 path
// correctly skipped `s.Count` (non-borrow-carrying field) and rooted only the
// span field's arg, but the §9.7 v0.301 ruling closes the parameter arm
// for that span field's arg. `N` stays untouched (still skipped — its arg
// is the value `s.Count`, not a borrow source). // per §9.7 v0.301; callee-
// side reportIfParameterRoot in Ownership_Checks.cpp's NewExpr arg scan.
BEZEL_TEST(record_direct_new_non_borrow_arg_2150,
           span_field_plus_int_from_local_length_rejected_9_7_v301) {
    std::string src =
        "module M; record Holder(S: ArraySpan<int>, N: int); class C "
        "  public static method View(p: Array<int>): Holder "
        "    var s := new Array<int>(); s.Add(11); "
        "    return new Holder(p.AsSpan(), s.Count); "
        "  end View; "
        "  end C;";
    auto r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned borrow-bearing record carries a borrow of parameter `p`"));
}

// §9.7 precision (#2150) / fix #4428 (§9.7 v0.301): same shape with the arg
// ORDER SWAPPED — the value field comes first, the borrow-carrying span
// second. Confirms the fix gates on the MATCHED FIELD (the §22.5 binding
// paths in Ownership_Checks.cpp's fieldArg[] lookup), not on argument
// position. Rejected on the span field's argument (`p.AsSpan()`, rooted at
// the parameter `p`) — `s.Count` (the value-typed `N` arg) is correctly
// skipped, not the reject source. // per §9.7 v0.301.
BEZEL_TEST(record_direct_new_non_borrow_arg_2150,
           int_from_local_length_plus_span_field_rejected_9_7_v301) {
    std::string src =
        "module M; record Holder(N: int, S: ArraySpan<int>); class C "
        "  public static method View(p: Array<int>): Holder "
        "    var s := new Array<int>(); s.Add(11); s.Add(13); "
        "    return new Holder(s.Count, p.AsSpan()); "
        "  end View; "
        "  end C;";
    auto r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned borrow-bearing record carries a borrow of parameter `p`"));
}

// §9.7 precision (#2150) / fix #4428 (§9.7 v0.301): a GENERIC Holder<T>
// instantiated with T=int for the value field and ArraySpan<int> for the
// span field — same shape as the above, with the subst env threaded through
// the field walk (#2145). Rejected on the span field's argument; the
// value-typed substitution (T=int) is correctly recognized as non-borrow-
// carrying, so the int arg `s.Count` is skipped. // per §9.7 v0.301.
BEZEL_TEST(record_direct_new_non_borrow_arg_2150,
           generic_value_field_plus_span_rejected_9_7_v301) {
    std::string src =
        "module M; record Holder<T>(V: T, S: ArraySpan<int>); class C "
        "  public static method View(p: Array<int>): Holder<int> "
        "    var s := new Array<int>(); s.Add(11); s.Add(13); s.Add(17); "
        "    return new Holder<int>(s.Count, p.AsSpan()); "
        "  end View; "
        "  end C;";
    auto r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned borrow-bearing record carries a borrow of parameter `p`"));
}
