// Ownership — fix #3102: the §30.5 NON-CHAINED `return <await borrow>;`
// rejection must teach the **kind-2 chained slot** (`return await task;`), not
// the generic §28.9/§9.7 borrow-escape remedy.
//
// Spec (revision v0.249):
//   §30.5 "Result lifetime and task disposal", producer-side rules, Pattern A
//     result slot kind 2 — "**`return await` over an owned Pattern A source —
//     the chained slot** … The result is a *borrow through the source task*:
//     ownership of the **source task** transfers into the outer task's state (a
//     **chained** slot), and the outer destructor disposes the chained source
//     task, never the borrowed result directly." For a `using var` source, "the
//     chained-slot arrangement **defers its disposal** from the frame's scope
//     end to the outer task's teardown".
//   §30.5, Consumer-side rules — the borrow produced by `await task` may flow to
//     "A `return` expression whose return type accepts a borrow with the same
//     source (e.g., `return await task;` from an async method returning Pattern
//     A `Task<T>`)."
//
// THE DEFECT (a residual recorded while fixing #3071; soundness already closed).
// #3071 made `checkIndirectEscapeOnReturn` fire on
//
//     using var task := C.P(); var cfg := await task; return cfg;
//
// but it fired through the SHARED generic message, whose remedy is "bind the
// source in the caller's scope and build the aliasing value there instead". For
// this shape that advice is both unnecessary and wrong-headed: no caller-side
// change is needed, and a caller cannot make a frame-local task outlive the
// frame. The admitted spelling is `return await task;` — the source task moves
// into the outer task's chained slot, which is what the author must be pointed
// at. Only the REMEDY clause forks; the diagnosis ("carries a borrow of `task`
// whose source is disposed at scope end") is unchanged, so the #3071 tests that
// match on it keep passing.
//
// These are diagnostic-quality tests: the shape was already rejected before this
// fix, so each rejection assertion below passes pre-fix — what FAILS pre-fix is
// the `kChainedSlotRemedy` expectation (the message said "bind the source in the
// caller's scope"), and what would fail a too-broad fix is the negative
// assertions on the non-async and Pattern-B-return controls.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// `Cfg` is a class, so `Task<Cfg>` is Pattern A: the await produces a BORROW of
// the task's result, sourced at the awaited task local (#3071).
std::string program(const std::string& body,
                    const std::string& sig = "M(): Task<Cfg>") {
    return std::string("module M; "
        "class Cfg public v: int; "
        "  public constructor(x: int) v := x; end constructor; "
        "end Cfg; "
        "class C "
        "  public static async method P(): Task<Cfg> "
        "    return new Cfg(42); "
        "  end P; "
        "  public static method Use(c: Cfg): int return c.v; end Use; "
        "  public static async method ") + sig + " " + body +
        " end M; end C;";
}

// The forked remedy: the kind-2 chained slot, naming the source task.
const char* kChainedSlotRemedy = "write `return await task;` instead";
const char* kChainedSlotCitation = "Pattern A chained slot (§30.5 kind 2)";
// The shared diagnosis clause, unchanged by the fork (#3071 matches on it).
const char* kCarriesBorrowMessage =
    "carries a borrow of `task` whose source is disposed at scope end";
// The generic §28.9/§9.7 remedy this shape must NO LONGER be given.
const char* kGenericRemedy = "bind the source in the caller's scope";

} // namespace

// THE FIX. Pattern A source local + Pattern A return type + async body: the
// rejection now names `return await task;` — the kind-2 chained slot — and no
// longer offers the caller-side-binding remedy, which does not apply.
BEZEL_TEST(await_chained_slot_remedy_3102, non_chained_return_points_at_kind2_30_5) {
    auto r = parseAndCheckOwnership(program(
        "using var task := C.P(); "
        "var cfg := await task; "
        "return cfg; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kChainedSlotRemedy));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kChainedSlotCitation));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kGenericRemedy));
}

// The DIAGNOSIS half is unchanged — only the remedy forked. Pins the clause the
// #3071 regression tests match on, so the two cannot drift apart.
BEZEL_TEST(await_chained_slot_remedy_3102, diagnosis_clause_unchanged_30_5) {
    auto r = parseAndCheckOwnership(program(
        "using var task := C.P(); "
        "var cfg := await task; "
        "return cfg; "));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kCarriesBorrowMessage));
}

// A PLAIN OWNED local source (not `using var`) is the other kind-2 source shape
// §30.5 admits ("an `owned` parameter or a plain owned local"), so the same
// remedy applies to it.
BEZEL_TEST(await_chained_slot_remedy_3102, plain_owned_local_source_points_at_kind2_30_5) {
    auto r = parseAndCheckOwnership(program(
        "var task := C.P(); "
        "var cfg := await task; "
        "return cfg; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kChainedSlotRemedy));
}

// CONTROL — the fix is a message fork, not an admission: the shape is still
// REJECTED. (Guards against a fork that accidentally swallowed the report.)
BEZEL_TEST(await_chained_slot_remedy_3102, shape_still_rejected_30_5) {
    auto r = parseAndCheckOwnership(program(
        "using var task := C.P(); "
        "var cfg := await task; "
        "return cfg; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// CONTROL — the ADMITTED kind-2 spelling the message now teaches must itself be
// accepted; a remedy the compiler rejects would be worse than the generic one.
BEZEL_TEST(await_chained_slot_remedy_3102, taught_chained_form_accepted_30_5) {
    auto r = parseAndCheckOwnership(program(
        "using var task := C.P(); "
        "return await task; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL — a NON-Pattern-A return type (`Task<int>`, a value result) is not a
// chained-slot position, so the fork must not fire there. Returning the borrow's
// int projection is clean; this pins that the value-result async body is
// untouched by the new predicate.
BEZEL_TEST(await_chained_slot_remedy_3102, value_result_return_not_forked_30_5) {
    auto r = parseAndCheckOwnership(program(
        "using var task := C.P(); "
        "var cfg := await task; "
        "return cfg.v; ", "M(): Task<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL — a SYNCHRONOUS (non-async) escape through the SAME check keeps its
// own remedy: a span holder escaping a scope-bound Array has no chained slot
// available. The fork is gated on `isAsync_` plus the two Pattern A type tests
// precisely so this path is untouched. (#1966's indirect span shape.)
BEZEL_TEST(await_chained_slot_remedy_3102, sync_span_escape_not_forked_9_7) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Leak(): ArraySpan<int> "
        "    using var local := new Array<int>(); local.Add(7); "
        "    var s := local.AsSpan(); "
        "    return s; "
        "  end Leak; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kChainedSlotRemedy));
}

// CONTROL — the discriminating half of the predicate: an async method that DOES
// return Pattern A `Task<Cfg>`, but whose escaping borrow's source is NOT a
// task (it is a `using var` Cfg). `return await c;` is not even well-typed
// there, so the chained-slot remedy must not be offered; the generic
// §28.9/§9.7 advice is the right one and is what still fires.
BEZEL_TEST(await_chained_slot_remedy_3102, non_task_source_keeps_generic_remedy_28_9) {
    auto r = parseAndCheckOwnership(program(
        "using var c := new Cfg(7); "
        "var alias := c; "
        "return alias; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, kChainedSlotRemedy));
}
