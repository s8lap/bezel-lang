// Return-atom holder of a borrow of an owned plain local escapes by `return`
// — heap-use-after-free (issue #4962; spec §17.15, §28.3 K8.4, v0.307).
//
// THE RULE. §17.15 / K8.4: "A borrow whose source is an atom binding may not
// escape the atom — in the body's value or any other holder — per K8.4:
// rejected, never lifetime-extended; bind at statement level (`using var`,
// §28.10) when the value must outlive the expression." The atom's value is
// its body's — `peelBindAtomToBody` makes this exact. The statement-form twin
// `var z := q; return z;` is correctly rejected ("returned `z` carries a
// borrow of `q` whose source is disposed at scope end", §28.3/§9.7); the
// atom-form `return (var z := q; z);` MUST be rejected with the SAME
// diagnostic and on the SAME kernel rule.
//
// THE BUG (#4962). The atom-clause binding `z` in `return (var z := q; z);`
// never had its borrow source recorded before handleReturn's escape checks
// fired — processBindAtom (the owner of the source-recording step for atom
// clauses) runs from checkRead → handleDischarge, which executes AFTER
// handleReturn has already consulted `states_[z].source` (empty) and the
// §28.3 indirect-escape check has silently accepted the escape. MEASURED
// pre-fix, `scripts/agent_check.sh --probe --asan` on r23-c28 (lane a63ccc3ca):
// heap-use-after-free in `bz$M.Widget$Name`. The source `q` is a fresh
// `new Widget("q")` — a plain owned local auto-disposed at the method's exit
// (§28.3 K8) — so `z`'s borrow of `q` outlives `q`'s scope, and the caller
// reads (and disposes) a freed `Widget`.
//
// OBSERVABILITY. The pre-fix behaviour is a CLEAN COMPILE then a UAF, so
// every REJECT row asserts the SPECIFIC §28.3 / §9.7 diagnostic AND
// `!r.ran`; the fix produces a compile-time reject with no runtime
// execution, so the absence of a UAF follows from the `!r.ran` arm.
//
// The fix: in `MethodOwnership::handleReturn` (Ownership.cpp ~line 8700),
// before `checkIndirectEscapeOnReturn(e)`, populate `states_[clause].source`
// from `borrowSourceForExpr(clause->initializer)` for every binding-atom
// clause reachable from the return expression (new helper
// `populateReturnAtomClauseBorrowSources` in Ownership_Checks.cpp). The
// §28.3 / §9.7 indirect-escape check then sees the carried source and
// rejects — compile-time, never runtime.
//
// per §17.15, §28.3 K8.4, §28.3 / §9.7 borrow-lifetime rule, v0.307

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// The exact diagnostic this fix produces — the same wording the
// statement-form twin `var z := q; return z;` already produces (the
// §28.3 / §9.7 borrow-lifetime rule).
const char* kBorrowEscapeNeedle =
    "carries a borrow of `q` whose source is disposed at scope end";

// `F`'s return type is RET and its body BODY. The Widget/H prelude mirrors
// the #2098 test's so the new assertions land in the same harness.
std::string program(const std::string& ret, const std::string& body) {
    return std::string(
               "module M; "
               "public class Widget "
               "  private _n: string; "
               "  public constructor(n: string) _n := n; end constructor; "
               "  destructor Console.WriteLine($\"dispose {_n}\"); "
               "  end destructor; "
               "  public method Name(): string => _n; "
               "end Widget; "
               "public class H "
               "  public constructor() end constructor; "
               "  public method F(c: bool, g: Widget): ") +
           ret + " " + body +
           "  end F; "
           "end H;";
}

}  // namespace

// ===========================================================================
// REJECT
// ===========================================================================

// T1 — THE ISSUE'S FIRST REPRO: `owned Widget` return over
// `(var z := q; z)` where q is a plain owned local. Pre-fix: clean compile,
// then heap-use-after-free in `bz$M.Widget$Name` after `q` is auto-dropped.
BEZEL_TEST(binding_atom_holder_return_escape_4962,
           spec_17_15_owned_return_binding_atom_holder_of_borrow_rejected) {
    auto r = parseAndCheckOwnership(program(
        "owned Widget",
        "var q := new Widget(\"q\"); "
        "return (var z := q; z);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kBorrowEscapeNeedle));
}

// T2 — THE ISSUE'S SECOND REPRO: plain (borrow) `Widget` return over the
// same atom body. The borrow return carries the same escape: the returned
// `z` is a borrow of `q` that outlives `q`'s scope. The diagnostic wording
// mirrors T1 (§28.3 / §9.7 borrow-lifetime rule).
BEZEL_TEST(binding_atom_holder_return_escape_4962,
           spec_17_15_borrow_return_binding_atom_holder_of_borrow_rejected) {
    auto r = parseAndCheckOwnership(program(
        "Widget",
        "var q := new Widget(\"q\"); "
        "return (var z := q; z);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kBorrowEscapeNeedle));
}

// T3 — The `??` form: `return (var z := p ?? q; z);` for completeness —
// §28.3 producer table's `a ?? b` row makes both arms borrows, so `z`'s
// source is the first escape-bound operand (here `q`).
BEZEL_TEST(binding_atom_holder_return_escape_4962,
           spec_17_15_coalesce_return_binding_atom_holder_of_borrow_rejected) {
    auto r = parseAndCheckOwnership(program(
        "owned Widget",
        "var p: Widget? := new Widget(\"p\"); "
        "var q := new Widget(\"q\"); "
        "return (var z := p ?? q; z);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // ?? form's first escape-bound operand is `p` (left, a `Widget?` whose
    // resolved value is `p`); the diagnostic names whichever arm carries
    // the escape. Both arms are non-null Widget instances in this test.
    BEZEL_EXPECT_TRUE(
        hasErrorMessage(r, "carries a borrow of `p`") ||
        hasErrorMessage(r, "carries a borrow of `q`"));
}
