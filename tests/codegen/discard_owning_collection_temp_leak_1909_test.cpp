// fix #1909 — §17.13 (the discard `_ :=`) + §28.3 (unbound owned temporaries)
// + §28.11 (owning-collection element cascade), spec v0.226. Codegen-observable,
// LeakSanitizer-checked coverage for: discarding a TEMPORARY (a CallExpr/
// NewExpr, never bound to a name) whose type is an owning collection
// (OwningArray/OwningDictionary/OwningLinkedList/…) leaked the container AND
// every owned element.
//
// This is the producer-TEMPORARY analogue of #1892 (discard_owning_collection_
// disposal_1892_1893_test.cpp), which covered the NAMED-BINDING discard form
// (`_ := arr;` for a named local `arr`). #1892's fix (edaed59) is deliberately
// keyed on the discard operand being an IdentifierExpr resolving to a
// LocalSymbol (discardOutOwnedVar) — a bare CallExpr/NewExpr temporary never
// reaches that path. Separately, the pre-existing #1009 producer-temporary
// discard path (disposeOwnedValue) is gated on the value's static type being a
// USER Class/Interface declaration — an owning collection is a decl-null
// BUILTIN, so that gate never fires either. Before this fix NEITHER path
// covered `_ := <call/new returning an owning collection>`: nothing disposed
// the container or its elements at all.
//
// §28.3 "Unbound owned temporaries": "an *unnamed* owned temporary — which no
// named form can reach — is reclaimed automatically once the expression that
// produced it is finished with it." §17.13's discard-of-a-producer bullet: "the
// discard is permitted and disposes the value at the end of the discard
// statement." §28.11 "Owning collections": "Disposing an OwningArray<T>
// automatically disposes every element — the cascade is the type's contract"
// (and the parallel language for OwningDictionary/OwningLinkedList). The fix
// (freeUnboundOwnedOwningCollectionTemp, gated by the new DiscardStmt::
// disposeOwnedCollectionValue flag) routes the discard through the same
// bz_array_free / bz_dict_free / bz_llist_free + element-cascade machinery a
// `using var` of the same type already gets (registerUsingDisposal), applied to
// a bare (value, type) pair instead of a LocalDecl.
//
// Every observable rides on a single `runAotChecked` run: the exit code encodes
// a destructor COUNT (class R bumps a static R._n on dispose), and the same
// AOT+ASan/LSan run flips `ran` to false on any leak / double-free / use-after-
// free. FAIL-WITHOUT/PASS-WITH: before the fix the leak legs return R._n == 0
// (with an LSan leak flag on the checked run); after the fix they return the
// correct disposed count with no leak.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Disposed-exactly-once oracle: a leak / double-free / use-after-free fails the
// checked run (ran=false -> -999), so the sanitizer verdict rides on the same
// assertion as the destructor-count exit code.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// A resource class R whose destructor bumps a static disposal counter R._n.
const char* kR =
    "class R "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end R; ";

// A Worker whose Step throws InvalidOperationException when x > 0 — used for
// the exception-path axis (mirrors #1892/#1893's kWorker).
const char* kWorker =
    "class W "
    "  public static method Step(x: int) "
    "    if x > 0 then throw new InvalidOperationException(\"boom\"); end; "
    "  end Step; "
    "end W; ";

std::string prog(const std::string& classesAfterR, const std::string& mainBody) {
    return std::string("module M; ") + kR + classesAfterR +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// ===========================================================================
// THE BUG — discard of a fresh `new OwningArray<R>()` TEMPORARY (never bound
// to a name) disposes the container and cascades into its owned elements, at
// the discard statement. FAIL-WITHOUT: R._n == 0 (+ leak of the struct, the
// backing buffer, AND both elements).
// ===========================================================================
BEZEL_TEST(discard_owning_collection_temp_1909, new_owning_array_temp_disposed_17_13_28_3) {
    const std::string src = prog(
        "",
        "  _ := new OwningArray<R>(); "
        "  return R._n; ");  // no elements added: container-only, 0 elements
    BEZEL_EXPECT_EQ(run(src), 0);
}

// The issue's own repro shape: `_ := <owned-returning STATIC method call>`
// producing OwningArray<R> — the sound gate is `producerState == Owned` via
// MethodDecl::returnOwned, not a name-whitelist, so a user factory hits the
// same path as a bare `new`.
BEZEL_TEST(discard_owning_collection_temp_1909, owned_returning_call_temp_disposed_28_3) {
    const std::string src = prog(
        "class Mk "
        "  public static method Make(): owned OwningArray<R> "
        "    var a := new OwningArray<R>(); "
        "    a.Add(new R()); "
        "    a.Add(new R()); "
        "    return a; "
        "  end Make; "
        "end Mk; ",
        "  _ := Mk.Make(); "
        "  return R._n; ");  // 2, disposed exactly once at the discard
    BEZEL_EXPECT_EQ(run(src), 2);
}

// ===========================================================================
// PRODUCER AXIS — an OwningDictionary<int,R> temp and an OwningLinkedList<R>
// temp cascade through their own runtime frees (bz_dict_free / bz_llist_free)
// exactly as the named-local #1892 fix does.
// ===========================================================================
BEZEL_TEST(discard_owning_collection_temp_1909, owning_dictionary_temp_disposes_values_28_11) {
    const std::string src = prog(
        "class Mk2 "
        "  public static method Make(): owned OwningDictionary<int, R> "
        "    var d := new OwningDictionary<int, R>(); "
        "    d.Add(1, new R()); "
        "    return d; "
        "  end Make; "
        "end Mk2; ",
        "  _ := Mk2.Make(); "
        "  return R._n; ");  // 1
    BEZEL_EXPECT_EQ(run(src), 1);
}

BEZEL_TEST(discard_owning_collection_temp_1909, owning_linked_list_temp_disposes_elements_28_11) {
    const std::string src = prog(
        "class Mk3 "
        "  public static method Make(): owned OwningLinkedList<R> "
        "    var ll := new OwningLinkedList<R>(); "
        "    ll.AddLast(new R()); "
        "    return ll; "
        "  end Make; "
        "end Mk3; ",
        "  _ := Mk3.Make(); "
        "  return R._n; ");  // 1
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// EXCEPTION-PATH AXIS — a throw between the temp's production and the discard
// statement is impossible to construct here (the temp IS the discard's whole
// expression — there is no operand evaluated between production and disposal
// within the SAME statement), so the control instead exercises a throw AFTER
// a clean discard has already run — verifying the free is not re-triggered.
// ===========================================================================
BEZEL_TEST(discard_owning_collection_temp_1909, no_double_free_when_throw_follows_discard_28_2) {
    const std::string src = prog(
        std::string(kWorker) +
        "class Mk4 "
        "  public static method Make(): owned OwningArray<R> "
        "    var a := new OwningArray<R>(); "
        "    a.Add(new R()); "
        "    return a; "
        "  end Make; "
        "end Mk4; "
        "class Run1 "
        "  public static method Go(): int "
        "    _ := Mk4.Make(); "   // disposes R._n -> 1, no lingering slot
        "    W.Step(1); "        // throws AFTER the discard
        "    return 0; "
        "  end Go; "
        "end Run1; ",
        "  var caught := 0; "
        "  try "
        "    _ := Run1.Go(); "
        "  catch InvalidOperationException => "
        "    caught := 1; "
        "  end; "
        "  return caught * 10 + R._n; ");  // 1*10 + 1 = 11, disposed once
    BEZEL_EXPECT_EQ(run(src), 11);
}

// ===========================================================================
// CONTROLS — non-collection discard shapes stay on their existing paths (no
// regression / no double-free introduced by the new disposeOwnedCollectionValue
// gate).
// ===========================================================================

// A named owning-collection LOCAL discard still goes through #1892's
// discardOutOwnedVar path (an IdentifierExpr operand), never the new
// disposeOwnedCollectionValue temp path — both gates are mutually exclusive
// (an IdentifierExpr is not a CallExpr/NewExpr producer). Disposed once.
BEZEL_TEST(discard_owning_collection_temp_1909, named_local_discard_unaffected_no_double_free_28_2) {
    const std::string src = prog(
        "",
        "  var arr := new OwningArray<R>(); "
        "  arr.Add(new R()); "
        "  _ := arr; "
        "  return R._n; ");  // 1, via the pre-existing #1892 path
    BEZEL_EXPECT_EQ(run(src), 1);
}

// A discard of a BORROWED owning collection must NOT be disposed — the
// collection is owned elsewhere. The field is later disposed once via the
// receiver's own owned-field cascade at scope end.
//
// 2026-07-29: this case previously sourced the borrow from a user method
// returning a non-`owned` owning collection (`public method Peek():
// OwningArray<R> return Items; end Peek;` then `_ := h.Peek();`). That shape
// stopped compiling when 342fdb9 (#1760) removed the receiver-owned-field
// carve-out for the owning-collection heads, so the test has been RED on
// origin/main since 2026-07-28 06:03.
//
// The shape is NOT restored here, because whether it should compile is an open
// spec-vs-implementation conflict, not a test bug: spec §28.6 admits it in
// prose ("applies uniformly to a class-typed owned field AND to an
// owning-collection field") and as a worked example — `public method
// GetTransactions(): OwningArray<Transaction> return _transactions;` — while
// #1760's rationale argues the borrow is unsound under interface /
// virtual-override / imported / function-value dispatch, where the caller
// cannot resolve the concrete method and disposes the borrow. Resolving that
// needs either a spec amendment or a sound call-site classification; both are
// owner decisions, tracked in #2269. Per CLAUDE.md, a spec defect is fixed by
// amending the spec, not by a test that blesses the divergence — so this test
// does not encode either side of it.
//
// What #1909 is actually about — that discarding a borrowed owning collection
// disposes nothing — is preserved by sourcing the borrow from a direct read of
// the owned field, which is admissible under both readings and exercises the
// same discard path.
BEZEL_TEST(discard_owning_collection_temp_1909, borrowed_owning_collection_discard_not_disposed_28_3) {
    const std::string src = prog(
        "class Holder "
        "  public owned Items: OwningArray<R>; "
        "  public constructor() "
        "    Items := new OwningArray<R>(); "
        "    Items.Add(new R()); "
        "  end constructor; "
        "end Holder; ",
        "  using var h := new Holder(); "
        "  _ := h.Items; "   // a BORROW of h.Items — discard disposes nothing
        "  return R._n; ");  // still 0 here — h's owned field disposes at scope end
    BEZEL_EXPECT_EQ(run(src), 0);
}

// A discard of a plain (non-collection, non-class-producing) VALUE — e.g. an
// int — is untouched by any of the owned-temp discard paths.
BEZEL_TEST(discard_owning_collection_temp_1909, plain_value_discard_unaffected) {
    const std::string src = prog(
        "",
        "  _ := 5; "
        "  return R._n; ");  // 0 — no R ever constructed
    BEZEL_EXPECT_EQ(run(src), 0);
}
