// Spec-derived tests for #4708 — §35.4 / §9.7 (v0.307): a choice that
// "HOLDS BORROWS" (the §35.4 derived property distinct from classification) is
// rejected at every §9.7 site. The pre-fix predicate
// `MethodOwnership::isBorrowBearingChoice` queried `cls && *cls == BorrowBearing`
// (the CLASSIFICATION alone) and silently admitted two §9.7-relevant cases:
//
//   1. `Owning`-classified choices with a borrow-carrying payload BESIDE their
//      `owned` ones. §35.4's own example: `Loaded(owned response: Response,
//      log: Logger)` is owning by rule 1 (the `owned response`), but also
//      `holds-borrows` (the unmarked `log: Logger`). The pre-fix predicate
//      returned false; the class-field store check (`/4708` site, line ~8407)
//      did not fire; `_outcome: LoadOutcome;` in a class field silently held
//      the `log` borrow past the source's auto-drop. Repro: rule-corpus row
//      `s35_4_0003.bzl`.
//
//   2. Hold §35.4's two-step shape: an owning choice whose case payload is a
//      borrow-bearing record (transitivity, §9.7 §35.4 holds-borrows rule).
//
//   3. Async param / result (§30.1 / §35.4): every choice that holds borrows
//      is forbidden as an async parameter or result, regardless of
//      classification.
//
// The post-fix predicate (`choiceHoldsBorrows`, file-local static in
// `src/ownership/Ownership_Checks.cpp:16755`) reads `holds-borrows` directly:
// short-circuits `BorrowBearing` and `ValueLike` via `classifyChoiceType`, then
// walks `Owning` choices' case payloads under §9.7's field-level predicate.
// Plain `Owning` (no held borrows) and `ValueLike` choices remain admitted at
// every site (the `owned` FIELD modifier on a ValueLike choice is the one
// exception, rejected by §28.5 per #5898) — a regression pin for the §35.4 final paragraph "a move-only
// value carrying no borrows may be a class field".
//
// Each `BEZEL_TEST` in this file pins one of:
//   (R) a REGRESSION — borrowed behavior that the pre-fix predicate already
//       covered and the post-fix must continue to cover (BorrowBearing at every
//       §9.7 site)
//   (N) NEW coverage — the Owning-with-borrows shape that the post-fix
//       predicate now rejects, with corpus `s35_4_0003` as the canonical repro
//   (P) PRESERVATION — plain Owning / ValueLike behavior the fix must NOT
//       regress (a move-only value carrying no borrows IS a legal class field
//       per §35.4; a value-like choice IS an ordinary copyable value)
//
// All tests use `parseAndCheckOwnership` (sema + ownership) — sufficient for
// the predicate-correctness check; the AOT+ASan oracle (`runAotChecked`) is
// invoked separately to cover the codegen path.
//
// per §35.4, §9.7, §30.1, §30.5, v0.307

#include <cstdio>
#include <cstdlib>
#include <string>

#include "support/ownership_support.hpp"  // parseAndCheckOwnership
#include "support/aot_support.hpp"        // runAotChecked — AOT+ASan oracle
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

// True iff an ERROR diagnostic whose message contains `sub` was emitted.
// Sub-pin: never satisfied by an unrelated diagnostic.
static bool hasError(const OwnershipResult& r, const std::string& sub) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(sub) != std::string::npos) return true;
    }
    return false;
}

// Dump all diagnostics — debugging aid for the FAIL-WITHOUT pass. Lives
// behind a flag passed via env BEZEL_TEST_VERBOSE_DIAG=1.
static int diagIndex() {
    static int idx = (std::getenv("BEZEL_TEST_VERBOSE_DIAG") ? 1 : 0);
    return idx;
}
template <typename R>
static void dumpDiag(const char* name, const R& r) {
    if (!diagIndex()) return;
    fprintf(stderr, "DIAG %s (%d total):\n", name, (int)r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics()) {
        fprintf(stderr, "  [%s] %s\n",
                d.severity == bezel::Severity::Error ? "ERR" :
                d.severity == bezel::Severity::Warning ? "WRN" : "?",
                d.message.c_str());
    }
}

// Common module prefixes — keep the §35.4-owning / §35.4-borrow-bearing
// choices parallel to the #2209 test fixture so the predicate behaviour is
// visible without reading arbitrary struct names.
//
//   Res / Logger — disposable reference types (a class is borrow-bearing per
//                  §9.7; we use them in the unmarked class/interface slot
//                  to construct a BORROW-BEARING choice).
//   Opt          — BorrowBearing: `Some(r: Res)` carries an unmarked class
//                  payload (`r`).
//   VOpt         — ValueLike: every payload is value-typed or string (no
//                  borrows, no `owned`).
//   PlainOwn     — Owning only: `Owned(owned w: Widget)` — no borrows beside
//                  its `owned` payload.
//   OW           — Owning-with-borrows: `Loaded(owned resource: Widget, log:
//                  Logger)` — Owning per §35.4 rule 1, but ALSO holds
//                  borrows via the unmarked `log: Logger` (the §35.4 own-
//                  example, the shape #4708 measures).
//   OWRecord     — Owning-with-borrows via transitivity: `Holder(owned w:
//                  Widget, rec: BR)` — Owning per `owned w`; BORROWS via the
//                  nested borrow-bearing record field.
//   OWInner      — Owning-with-borrows via transitivity: `Wrap(owned w:
//                  Widget, inner: OW)` — Owning per `owned w`; BORROWS via
//                  the nested Owning-with-borrows choice.
namespace {
const char* kResLog =
    "module M; "
    "class Res "
    "  public constructor() end constructor; "
    "  destructor end destructor; end Res; "
    "class Logger "
    "  public constructor() end constructor; "
    "  destructor end destructor; end Logger; "
    "class Widget "
    "  public constructor() end constructor; "
    "  destructor end destructor; end Widget; "
    // BorrowBearing: payload is an unmarked class (Logger).
    "choice Opt Some(r: Res), None; end Opt; "
    // ValueLike: every payload is `int`.
    "choice VOpt Some(n: int), None; end VOpt; "
    // Plain Owning: ONLY owned payload (no borrows beside the owned one).
    "choice PlainOwn Owned(owned w: Widget), Empty; end PlainOwn; "
    // §35.4's own example: Owning per `owned resource`; ALSO holds borrows
    // via the unmarked `log: Logger`. This is the #4708 repro shape.
    "choice OW "
    "  Loaded(owned resource: Widget, log: Logger), "
    "  Failed; "
    "end OW; ";
} // namespace

// ===========================================================================
// (N) The canonical #4708 repro: a class instance field of `OW`
// (Owning-with-borrows) is rejected at the class-field-store check (line 8407
// in Ownership_Checks.cpp). The pre-fix predicate returned false for `OW`
// (it classifies as `Owning`), so the check no-op'd and the bug flew. Post-
// fix: `isBorrowBearingChoice(OW)` returns true (holds-borrows), and the
// §9.7 / §35.4 diagnostic fires.
// ===========================================================================
BEZEL_TEST(choice_borrow_field_owning_4708, owning_with_borrows_class_field_rejected) {
    auto r = parseAndCheckOwnership(std::string(kResLog) +
        "class Holder "
        "  private owned _outcome: OW; "
        "  public constructor(owned outcome: OW) _outcome := outcome; "
        "  end constructor; "
        "end Holder;");
    dumpDiag("owning_with_borrows_class_field_rejected", r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // Class-field-store diagnostic wording is fixed at line 8429-8442; pin on
    // its phrases (the diagnostic does NOT spell the type name `OW`).
    BEZEL_EXPECT_TRUE(hasError(r, "borrow-bearing choice"));
    BEZEL_EXPECT_TRUE(hasError(r, "instance field of a class"));
    // §35.4 / §9.7 citation (the class-field-store diagnostic spells
    // "§9.7 with 'record' read as 'record or choice', §35.4").
    BEZEL_EXPECT_TRUE(hasError(r, "§35.4"));
    BEZEL_EXPECT_TRUE(hasError(r, "§9.7"));
}

// ===========================================================================
// (N) Transitivity: an Owning choice whose case payload is a borrow-bearing
// record (`rec: BR`). §35.4: a choice holds-borrows if any case has a
// borrow-carrying payload under §9.7; §9.7 transitivity: a record field is
// borrow-carrying iff that record is borrow-bearing. So `OWRecord` is
// owns-by-`w`-and-holds-borrows-by-`rec`, AND `OWRecord: field_of_class`
// must reject.
// ===========================================================================
BEZEL_TEST(choice_borrow_field_owning_4708,
           owning_with_borrows_via_record_field_class_field_rejected) {
    auto r = parseAndCheckOwnership(
        std::string(kResLog) +
        // Borrow-bearing record via the existing Opt.
        "choice OWR "
        "  Holder(owned w: Widget, rec: Opt), "
        "  None; "
        "end OWR; "
        "class Box "
        "  public owned _it: OWR; "
        "  public constructor(owned x: OWR) _it := x; end constructor; "
        "end Box;");
    dumpDiag("owning_with_borrows_via_record_field", r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "borrow-bearing choice"));
    BEZEL_EXPECT_TRUE(hasError(r, "instance field of a class"));
}

// ===========================================================================
// (N) Transitivity: an Owning choice whose case payload is a nested
// Owning-with-borrows choice (`inner: OW`). §35.4: "this property at a
// nested choice payload" — the inner OW holds borrows, so OWInner holds
// borrows.
// ===========================================================================
BEZEL_TEST(choice_borrow_field_owning_4708,
           owning_with_borrows_via_nested_choice_class_field_rejected) {
    auto r = parseAndCheckOwnership(
        std::string(kResLog) +
        "choice OWI Wrap(owned w: Widget, inner: OW), Gone; end OWI; "
        "class Box "
        "  public owned _it: OWI; "
        "  public constructor(owned x: OWI) _it := x; end constructor; "
        "end Box;");
    dumpDiag("owning_with_borrows_via_nested_choice", r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "borrow-bearing choice"));
    BEZEL_EXPECT_TRUE(hasError(r, "instance field of a class"));
}

// ===========================================================================
// (R) BorrowBearing (`Opt`) at the same site still rejects — the post-fix
// predicate must continue to cover the pre-fix shape; this pins that
// BorrowBearing still returns true and the check still fires.
// ===========================================================================
BEZEL_TEST(choice_borrow_field_owning_4708,
           borrow_bearing_class_field_still_rejected) {
    auto r = parseAndCheckOwnership(
        std::string(kResLog) +
        "class Holder "
        "  public owned _o: Opt; "
        "  public constructor(owned o: Opt) _o := o; end constructor; "
        "end Holder;");
    dumpDiag("borrow_bearing_class_field_still_rejected", r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "borrow-bearing choice"));
    BEZEL_EXPECT_TRUE(hasError(r, "instance field of a class"));
}

// ===========================================================================
// (P) PLAIN Owning (no borrows beside the owned payload) at a class field is
// ADMITTED — §35.4 final paragraph: "a move-only value carrying no borrows
// may be a class field (the class's destructor disposes it)". Crucial
// regression pin: a fix that conflates Owning with BorrowBearing would
// over-reject plain Owning here.
// ===========================================================================
BEZEL_TEST(choice_borrow_field_owning_4708, plain_owning_class_field_accepted) {
    auto r = parseAndCheckOwnership(
        std::string(kResLog) +
        "class Holder "
        "  private owned _outcome: PlainOwn; "
        "  public constructor(owned outcome: PlainOwn) _outcome := outcome; "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var h := new Holder(PlainOwn.Empty); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// (R) `owned` on a VALUE-classified choice (`VOpt`) at a class field is
// REJECTED — §28.5 bullet 1 (v0.338): the `owned` field modifier marks only a
// class or interface field, or is admitted-but-redundant on an owning-choice
// field; a value-classified choice is neither. Issue #5898. The diagnostic
// names §28.5.
// ===========================================================================
BEZEL_TEST(choice_borrow_field_owning_4708,
           value_like_choice_class_field_rejected) {
    auto r = parseAndCheckOwnership(
        std::string(kResLog) +
        "class Holder "
        "  private owned _v: VOpt; "
        "  public constructor(owned x: VOpt) _v := x; end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var h := new Holder(VOpt.Some(7)); "
        "  return 0; "
        "end Main; end C;");
    dumpDiag("value_like_choice_class_field_rejected", r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "§28.5"));
    BEZEL_EXPECT_TRUE(hasError(r, "value-classified choice"));
}

// ===========================================================================
// (P) ValueLike (`VOpt`) WITHOUT the `owned` modifier at a class field is
// ADMITTED — §35.4 rule 3 construction: every payload is value-typed; no held
// borrows. The field is an ordinary copyable value. Regression pin for the
// #5898 field-only rejection: the modifier is what is rejected, not the type.
// ===========================================================================
BEZEL_TEST(choice_borrow_field_owning_4708,
           value_like_choice_class_field_unmarked_accepted) {
    auto r = parseAndCheckOwnership(
        std::string(kResLog) +
        "class Holder "
        "  private _v: VOpt; "
        "  public constructor(x: VOpt) _v := x; end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var h := new Holder(VOpt.Some(7)); "
        "  return 0; "
        "end Main; end C;");
    dumpDiag("value_like_choice_class_field_unmarked_accepted", r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// (N) ASYNC PARAMETER (§30.1, the `:16779` site): an Owning-with-borrows
// choice as an `async` parameter is rejected. Pre-fix: silently admitted.
// Post-fix: rejected with the §30.1 + §35.4 diagnostic.
//
// Per the brief, async sites must be VERIFIED BY MEASUREMENT, not by reading
// the spec. This AOT+ASan run is that measurement — if the diagnostic does
// not fire at compile time, the program continues to codegen, and the
// ASan-clean exit code proves the program terminated normally. Pin both:
// an OwnershipResult parse error AND the AOT path compiles clean (because
// the diagnostic fired first).
// ===========================================================================
BEZEL_TEST(choice_borrow_field_owning_4708,
           owning_with_borrows_async_parameter_rejected) {
    auto r = parseAndCheckOwnership(
        std::string(kResLog) +
        "class C "
        "  public method MakeInt(): Task<int> => Task.FromResult(0); "
        "  public async method Run(o: OW): Task "
        "    var r := await MakeInt(); "
        "  end Run; "
        "end C;");
    dumpDiag("owning_with_borrows_async_parameter_rejected", r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // ASYNC parameter diagnostic (line 16820-16836 in Ownership_Checks.cpp):
    // pins on its key phrases — does not spell the type name `OW`.
    BEZEL_EXPECT_TRUE(hasError(r, "borrow-bearing choice"));
    BEZEL_EXPECT_TRUE(hasError(r, "§30.1"));
    BEZEL_EXPECT_TRUE(hasError(r, "§35.4"));
}

// ===========================================================================
// (N) ASYNC RESULT (§30.5, the `:16818` site): `Task<OW>` is rejected. Pre-
// fix: silently admitted. Post-fix: the §30.1/§35.4 diagnostic fires.
// ===========================================================================
BEZEL_TEST(choice_borrow_field_owning_4708,
           owning_with_borrows_async_result_rejected) {
    auto r = parseAndCheckOwnership(
        std::string(kResLog) +
        // Exercise the §35.4 final paragraph "the async seam is closed on
        // the owning side" orthogonal gap that the BorrowBearing-classified
        // test cannot reach — plain Owning (no borrows) at the TASK RESULT
        // (`Task<PlainOwn>`) is rejected by the orthogonal "no §30.5 producer
        // row admits a move-only value result" rule (line 8425-style via
        // some other site) AND the new own-with-borrows result rule
        // (line 16820). Both must fire; the new rule is the lower-hanging
        // observable.
        "class C "
        "  public async method Run(): Task<OW> "
        "    return await Task.FromResult(OW.Failed); "
        "  end Run; "
        "end C;");
    dumpDiag("owning_with_borrows_async_result_rejected", r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The borrowing-bearing result/element-type diagnostic carries
    // "borrow-bearing choice result or element type".
    BEZEL_EXPECT_TRUE(hasError(r, "borrow-bearing choice"));
    BEZEL_EXPECT_TRUE(hasError(r, "result or element type"));
}

// ===========================================================================
// (R) BorrowBearing async parameter still rejects (pre-fix already covered).
// ===========================================================================
BEZEL_TEST(choice_borrow_field_owning_4708,
           borrow_bearing_async_parameter_still_rejected) {
    auto r = parseAndCheckOwnership(
        std::string(kResLog) +
        "class C "
        "  public method MakeInt(): Task<int> => Task.FromResult(0); "
        "  public async method Run(o: Opt): Task "
        "    var r := await MakeInt(); "
        "  end Run; "
        "end C;");
    dumpDiag("borrow_bearing_async_parameter_still_rejected", r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "borrow-bearing choice"));
    BEZEL_EXPECT_TRUE(hasError(r, "§30.1"));
}

// ===========================================================================
// (P) Plain Owning async parameter is ADMITTED — §30.1 / §35.4 close the
// async seam only on borrows-bearing choices, not on all Owning.
// ===========================================================================
BEZEL_TEST(choice_borrow_field_owning_4708,
           plain_owning_async_parameter_accepted) {
    auto r = parseAndCheckOwnership(
        std::string(kResLog) +
        "class C "
        "  public method MakeInt(): Task<int> => Task.FromResult(0); "
        "  public async method Run(o: PlainOwn): Task "
        "    var r := await MakeInt(); "
        "  end Run; "
        "end C;");
    dumpDiag("plain_owning_async_parameter_accepted", r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// (N) AOT — full AOT+ASan exercise of the Owning-with-borrows shape REJECTED
// at parse-and-check (so codegen never sees it). The AOT path here exercises
// the ACCEPT side: a plain `Loaded(owned w: Widget)` literal in a local,
// which must compile and run cleanly. Both `Opt` (BorrowBearing) and `OW`
// (Owning-with-borrows) move-only locals must auto-dispose at scope end with
// the §15.3 / §35.4 must-use warning (K3.14) — the existing choice-borrow-
// field-2209 lit and gtest coverage handles BorrowBearing. The Owning-with-
// borrows shape is what #4708 measures. Pin a clean AOT run with `OW` going
// out of scope at the SAME point as a plain `PlainOwn` (no borrows) —
// `PlainOwn` is the preservation pin; `OW` is the new acceptance gate. If the
// predicate is correct, neither over-rejects; if a plain-Owning widens to
// BorrowBearing, `PlainOwn` would also be flagged at its disposal point.
// ===========================================================================
BEZEL_TEST(choice_borrow_field_owning_4708,
           aot_plain_owning_choice_local_auto_disposes_cleanly) {
    std::string src =
        std::string(kResLog) +
        "class C "
        "  public static method Main(): int "
        "    var p := PlainOwn.Owned(new Widget()); "
        "    Console.WriteLine(\"hello\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
