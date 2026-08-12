// Ownership — fix #3071: an await-produced Pattern A borrow recorded NO borrow
// source, so §28.3's borrow-outlives-source rejection could never fire for it.
//
// Spec (revision v0.249):
//   §30.5 "Result lifetime and task disposal", Consumer-side rules — "The borrow
//     produced by `await task` (where `task` occupies one of the admitted
//     positions) may flow to: - A `var` binding: `var r := await task;` — `r` is
//     a non-owned local **with the same source as `task`**. - A non-owned
//     argument: `Process(await task);`. - A `return` expression whose return
//     type accepts a borrow with the same source (e.g., `return await task;`
//     from an async method returning Pattern A `Task<T>`)."
//   §28.3 "Borrow lifetime rules" — "A borrow may not be assigned to a local
//     whose scope strictly encloses the borrow's source's scope"; the compiler's
//     own message is "Borrow held by `x` would outlive its source `y`."
//
// THE DEFECT. `MethodOwnership::borrowSourceForExpr` dispatched on
// `IdentifierExpr`, `MemberAccessExpr`, `NullCondMemberExpr`, `IndexExpr`,
// `NewExpr` and catch bindings — there was no `UnaryExpr`/`await` arm. So
// `var cfg := await task;` recorded no source at all, `states_[cfg].source`
// stayed empty, and every consumer of that field — the assignment-store escape
// check and the indirect-return escape check — was STRUCTURALLY unable to fire.
// Both onward flows §30.5 bars were silently accepted and both dangled:
//
//     do using var task := P(); var cfg := await task; w := cfg; end;
//     return w.v;                        -- reads freed memory
//
//     using var task := P(); var cfg := await task; return cfg;   -- SIGSEGV
//
// "with the same source as `task`" is the load-bearing clause: recording that
// source is the whole fix, after which the pre-existing §28.3 rejections do the
// work. The `.Result` sibling read (`var cfg := task.Result;`) already reached
// the `MemberAccessExpr` arm and was already rejected — the `await` spelling was
// the only one of the pair without a source, which is what
// `result_read_sibling_already_rejected_30_3` below pins.
//
// These are diagnostic tests; the run-time consequence is what the issue's ASan
// reports recorded (heap-use-after-free on the first shape, SIGSEGV on the
// second).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// `Cfg` is a class, so `Task<Cfg>` is Pattern A: the await produces a BORROW of
// the task's result. `Wide` is the enclosing-scope sink for the store shape.
std::string program(const std::string& body,
                    const std::string& sig = "M(): Task") {
    return std::string("module M; "
        "class Cfg public v: int; "
        "  public constructor(x: int) v := x; end constructor; "
        "end Cfg; "
        "class C "
        "  public static async method P(): Task<Cfg> "
        "    return new Cfg(42); "
        "  end P; "
        "  public static method Use(c: Cfg): int return c.v; end Use; "
        "  public static async method ") + sig + " " + body + " end M; "
        "end C;";
}

// A SYNCHRONOUS host for the `.Result` sibling — §30.3's blocking read is
// rejected inside an `async` body.
std::string syncProgram(const std::string& body) {
    return std::string("module M; "
        "class Cfg public v: int; "
        "  public constructor(x: int) v := x; end constructor; "
        "end Cfg; "
        "class C "
        "  public static async method P(): Task<Cfg> "
        "    return new Cfg(42); "
        "  end P; "
        "  public static method M(): int " + body + " return 0; end M; "
        "end C;");
}

const char* kOutlivesMessage = "would outlive its source";
const char* kCarriesBorrowMessage = "carries a borrow of";

} // namespace

// §30.5 / §28.3: the borrow's source is the awaited task local, so storing it
// into a STRICTLY-WIDER-scope local lets it outlive that source. Rejected.
// Pre-fix: silently accepted; `w.v` then read freed memory.
BEZEL_TEST(await_borrow_source_3071, store_into_wider_scope_local_rejected_28_3) {
    auto r = parseAndCheckOwnership(program(
        "var w: Cfg; "
        "do "
        "  using var task := C.P(); "
        "  var cfg := await task; "
        "  w := cfg; "
        "end; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kOutlivesMessage));
}

// §30.5: a NON-CHAINED `return cfg;` returns the borrow while the source task
// stays scope-bound to this frame and is disposed at its scope end. This is not
// the kind-2 chained slot (that is `return await <source>;`, which moves the
// source task into the outer task's chained slot). Rejected. Pre-fix: silently
// accepted, and the caller's read SIGSEGV'd.
BEZEL_TEST(await_borrow_source_3071, non_chained_return_of_borrow_rejected_30_5) {
    auto r = parseAndCheckOwnership(program(
        "using var task := C.P(); "
        "var cfg := await task; "
        "return cfg; ", "M(): Task<Cfg>"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kCarriesBorrowMessage));
}

// The same store, but through the `owned` PARAMETER spelling of an admitted
// operand (§30.5's second admitted position): the borrow's source is the
// parameter, which spans the whole body, so nothing outlives it. Clean.
BEZEL_TEST(await_borrow_source_3071, owned_param_source_store_clean_30_5) {
    auto r = parseAndCheckOwnership(program(
        "var w: Cfg; "
        "do "
        "  var cfg := await t; "
        "  w := cfg; "
        "end; "
        "Use(w); ", "M(owned t: Task<Cfg>): Task"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: a store into a local in the SAME scope as the source is admitted —
// the new source is recorded, not blanket-rejected. (§28.3 rejects only a
// strictly-enclosing target scope.)
BEZEL_TEST(await_borrow_source_3071, store_into_same_scope_local_clean_28_3) {
    auto r = parseAndCheckOwnership(program(
        "using var task := C.P(); "
        "var w: Cfg; "
        "var cfg := await task; "
        "w := cfg; "
        "Use(w); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: §30.5's first two admitted flows — a `var` binding and a non-owned
// argument — inside the source's own scope. Clean.
BEZEL_TEST(await_borrow_source_3071, var_bind_and_borrow_arg_clean_30_5) {
    auto r = parseAndCheckOwnership(program(
        "using var task := C.P(); "
        "var cfg := await task; "
        "var n := Use(cfg); "
        "var alias := cfg; "
        "var k := Use(alias); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: a VALUE projection off the borrow is a copy, not the borrow (§28.3
// "value-typed returns"), so returning it is not an escape. Clean.
BEZEL_TEST(await_borrow_source_3071, value_projection_return_clean_28_3) {
    auto r = parseAndCheckOwnership(program(
        "using var task := C.P(); "
        "var cfg := await task; "
        "return cfg.v; ", "M(): Task<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: an UNBOUND producer operand records NO source — `identifierRoot`
// peels a call to its receiver, and the receiver is not the borrow's source. The
// escaping bind is already the §30.5 rejection (a different rule), so this pins
// that the new arm did not start attributing a temporary's borrow to a receiver.
BEZEL_TEST(await_borrow_source_3071, unbound_producer_operand_confined_clean_30_5) {
    auto r = parseAndCheckOwnership(program(
        "var n := Use(await C.P()); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The `.Result` sibling of the first shape was ALREADY rejected before this fix:
// `task.Result` is a MemberAccessExpr, which `borrowSourceForExpr` already
// handled, so it already carried `task` as its source. Pinned here as the parity
// the `await` spelling was missing — and so the pair cannot drift apart again.
// // per §30.3, §30.5, §28.3
BEZEL_TEST(await_borrow_source_3071, result_read_sibling_already_rejected_30_3) {
    auto r = parseAndCheckOwnership(syncProgram(
        "var w: Cfg; "
        "do "
        "  using var task := C.P(); "
        "  var cfg := task.Result; "
        "  w := cfg; "
        "end; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kOutlivesMessage));
}
