// An owned iterable-source local moved into an `owned` parameter is still
// disposed by the CALLER at scope end — a heap-use-after-free when the callee
// disposes its owned parameter (issue #4794; spec §28.3, §28.8, §28.8.1,
// §29.1; stdlib §4.13, v0.307).
//
// THE RULE. §28.8.1: `move x` transfers the value to the new owner and leaves
// `x` moved-from; §28.3's discharge machinery skips a moved-from local, and
// Appendix C's dispose-exactly-once invariant forbids the old owner disposing
// it again. A callee that receives the value through an `owned` parameter may
// discharge it however it likes — an explicit `src.Dispose()` (§28.8) or its
// own scope-end reclaim.
//
// THE BUG (#4794). `var view := Gen();` (an `IOwningEnumerable<W>` iterator
// method) is claimed for scope-end disposal by codegen's
// `registerIterableSourceDisposal` (a null-guarded slot-1 Dispose of the
// local's slot). The `move view` lowering nulled the source slot only for
// `using var` / iteration-variable / out-owned / arm-transfer / armed plain-
// `var` locals — never for this claim — so the caller's scope-end Dispose ran
// on the frame the callee had already freed. MEASURED on r20-c25 (9e254617),
// `scripts/agent_check.sh --probe --asan` on the issue repro:
// heap-use-after-free in `bz$...$Moved`, freed in `...$Take`.
//
// ORACLE. Each row runs the transfer 100 times and prints an exact
// dispose count (loop-amplified so a single lucky layout cannot pass), under
// runAotChecked's generated-code ASan leg (a use-after-free fails the row).
//
// LEAK SCOPE. The three `Gen()` rows use runAotCheckedLenient (LSan off,
// ASan on): the CALLEE side of this shape leaks independently of #4794 —
// an explicit `src.Dispose()` on an IOwningEnumerable-typed yield frame
// dispatches the class vtable's slot 2 (MoveNext) instead of Dispose
// (#4827), and no other discharge of such an owned parameter reclaims the
// frame (#4828). Both reproduce with no caller-side `move` at all
// (`Take(Gen())`), so they are not this fix's to pin; the UAF this fix
// removes stays fully visible to the ASan leg. The drain-view row and the
// control are leak-clean and use the strict runAotChecked. When #4827 lands,
// switch these rows back to runAotChecked.
//
// per §28.3, §28.8, §28.8.1, §29.1, stdlib §4.13, v0.307

#include <string>

#include "support/aot_support.hpp"  // runAotChecked — strict AOT
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

const char* kDecls =
    "class W "
    "  public static _n: int := 0; "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  destructor _n := _n + 1; end destructor; "
    "end W; ";

// The callee that DISPOSES its owned parameter explicitly (the issue's Take).
const char* kTakeDisposes =
    "  public static method Take(owned src: IOwningEnumerable<W>): int "
    "    var n := 0; "
    "    for var x in src do "
    "      using var y := move x; "
    "      n := n + y.Id; "
    "    end; "
    "    src.Dispose(); "
    "    return n; "
    "  end Take; ";

const char* kGen =
    "  public static method Gen(): IOwningEnumerable<W> "
    "    yield return new W(1); yield return new W(2); yield return new W(3); "
    "  end Gen; ";

std::string prog(const std::string& members) {
    return std::string("module P5; ") + kDecls +
           "class C " + members +
           "  public static method Main(): int "
           "    var s := 0; "
           "    for var i in 0..<100 do s := s + Moved(); end; "
           "    Console.WriteLine($\"s={s} disposed={W._n}\"); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

void expectRun(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// (1) THE ISSUE'S REPRO: statement-form move into an owned parameter whose
// callee disposes it explicitly. Pre-fix: heap-use-after-free in Moved.
BEZEL_TEST(owned_iterable_local_move_to_owned_param_4794,
           spec_28_8_1_issue_repro_moved_view_not_disposed_by_caller) {
    auto r = runAotCheckedLenient(prog(std::string(kTakeDisposes) + kGen +
        "  public static method Moved(): int "
        "    var view := Gen(); "
        "    var r := Take(move view); "
        "    return r; "
        "  end Moved; "));
    expectRun(r, "s=600 disposed=300\n");
}

// (2) GUARD — the move sits in a RETURN expression. MEASURED pre-fix this
// shape was already UAF-free (the return edge does not run the claim for a
// local its expression consumed), so this row passes on both sides; it
// pins that the new slot-null does not disturb the return-edge path.
BEZEL_TEST(owned_iterable_local_move_to_owned_param_4794,
           guard_moved_view_in_return_expression_not_disposed_by_caller) {
    auto r = runAotCheckedLenient(prog(std::string(kTakeDisposes) + kGen +
        "  public static method Moved(): int "
        "    var view := Gen(); "
        "    return Take(move view) + 0; "
        "  end Moved; "));
    expectRun(r, "s=600 disposed=300\n");
}

// (3) A CHAINED transfer: the first callee forwards its owned parameter
// with a second `move` to the callee that disposes it. The caller's local
// and the intermediate parameter each hand the frame on; exactly one
// disposal in total.
BEZEL_TEST(owned_iterable_local_move_to_owned_param_4794,
           spec_28_8_1_chained_move_of_view_through_owned_param_exactly_once) {
    auto r = runAotCheckedLenient(prog(std::string(
        "  public static method Take(owned src: IOwningEnumerable<W>): int "
        "    var n := 0; "
        "    for var x in src do "
        "      using var y := move x; "
        "      n := n + y.Id; "
        "    end; "
        "    src.Dispose(); "
        "    return n; "
        "  end Take; "
        "  public static method Sum(owned src: IOwningEnumerable<W>): int "
        "    return Take(move src); "
        "  end Sum; ") + kGen +
        "  public static method Moved(): int "
        "    var view := Gen(); "
        "    var r := Sum(move view); "
        "    return r; "
        "  end Moved; "));
    expectRun(r, "s=600 disposed=300\n");
}

// (4) The DRAIN-VIEW instance (stdlib §4.13): `Drain()` mints a fresh
// caller-owned frame (#4772) that the named local claims; moving it into
// the owned parameter must hand that claim over too. Return-expression
// form (the issue notes the statement form is sema-rejected separately).
BEZEL_TEST(owned_iterable_local_move_to_owned_param_4794,
           stdlib_4_13_moved_drain_view_not_disposed_by_caller) {
    auto r = runAotChecked(prog(std::string(kTakeDisposes) +
        "  public static method Moved(): int "
        "    using var pool := new OwningArray<W>(); "
        "    pool.Add(new W(1)); pool.Add(new W(2)); pool.Add(new W(3)); "
        "    var view := pool.Drain(); "
        "    return Take(move view) + pool.Count; "
        "  end Moved; "));
    expectRun(r, "s=600 disposed=300\n");
}

// (5) CONTROL — the un-moved local keeps its scope-end claim: iterated in
// place and never transferred, the frame is disposed exactly once by the
// caller (no leak introduced by the fix).
BEZEL_TEST(owned_iterable_local_move_to_owned_param_4794,
           control_unmoved_view_still_disposed_at_scope_end) {
    auto r = runAotChecked(prog(std::string(kGen) +
        "  public static method Moved(): int "
        "    var view := Gen(); "
        "    var n := 0; "
        "    for var x in view do "
        "      using var y := move x; "
        "      n := n + y.Id; "
        "    end; "
        "    return n; "
        "  end Moved; "));
    expectRun(r, "s=600 disposed=300\n");
}
