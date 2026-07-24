// fix #1859 — §28.3 (unbound owned temporaries / producers table), spec
// v0.226. Codegen-observable, LeakSanitizer-checked coverage for: an UNBOUND
// RECEIVER temporary of a destructorless class leaked its heap block when the
// call made through it returned an `owned` CLASS/INTERFACE value.
//
// This is a gap in disposeFreshNewReceiverTemp (#1039/#1053, §28.3), the
// codegen helper that frees a fresh `new <class>()` (or `owned`-returning-call)
// temporary used ONLY as a receiver — never bound to a name. Before this fix
// the helper's `valueResult` gate admitted only a NON-REFERENCE extracted
// result (primitive/enum/unit): a class/interface result was unconditionally
// excluded, on the theory that it "may embed a borrow of the receiver" — sound
// for the ORDINARY (unmarked) instance-method-return row ("Borrowed — instance
// method: source is the receiver", §28.3 Producers table), but NOT for a call
// whose declared return is `owned`: an announced `owned` return is a FRESH
// TRANSFER (K1.5/K3.13, "Fresh productions are owned: … owned-returning
// calls") that carries no SIGNATURE-LEVEL borrow of the receiver — only a
// deferred-activation return type (Task family, an iterator interface)
// announces a carried receiver borrow (K6.2), and a plain `owned` class/
// interface return is not one of those kinds. The receiver was already due
// for disposal at the end of the full expression (§28.3's unbound-temporary
// rule — its only use was as the call's receiver, a borrowing use); the fix
// extends the `valueResult` gate to admit a class/interface result when the
// CALL's own declared return is `owned` (ast::MethodDecl::returnOwned),
// mirroring the exact discriminant isFreshOwnedClassProducer already uses to
// recognize the RECEIVER side of the same call as a fresh producer.
//
// Every observable rides on a single `runAotChecked` run: the exit code
// encodes a destructor COUNT (a static counter bumped on dispose), and the
// same AOT+ASan/LSan run flips `ran` to false on any leak / double-free /
// use-after-free. FAIL-WITHOUT/PASS-WITH: before the fix the receiver's
// destructor never runs and its block leaks (LSan flag); after the fix it is
// disposed exactly once with no leak.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ===========================================================================
// THE BUG (issue's own repro shape) — `new Box().Wrap(new Conn(7))`: the
// receiver `new Box()` is destructorless (no user destructor, no owned
// fields — trivial-dispose per §28.9, still a real heap block to reclaim) and
// unbound; `Wrap` returns `owned Conn`. Before the fix: Conn disposed once
// (Box._n unaffected by Conn's own counter), but Box's block leaked (LSan:
// 16 bytes). Box._n bumps on CONSTRUCTION only (no destructor to bump a
// disposal counter — destructorless is the point) — this test instead relies
// on the LSan leg (via runAotChecked) to prove the block IS reclaimed; the
// exit code independently proves Conn's normal disposal is untouched.
// ===========================================================================
BEZEL_TEST(fresh_owned_receiver_1859, class_result_receiver_disposed_28_3) {
    const std::string src =
        "module M; "
        "class Conn "
        "  public static _n: int := 0; "
        "  public Id: int; "
        "  public constructor(id: int) Id := id; end constructor; "
        "  destructor _n := _n + 1; end destructor; "
        "end Conn; "
        "class Box "                                    // destructorless
        "  public method Wrap(owned c: Conn): owned Conn return c; end Wrap; "
        "end Box; "
        "class P "
        "  public static method Main(): int "
        "    using var c := new Box().Wrap(new Conn(7)); "
        "    return Conn._n; "   // Conn not yet disposed here (c still owns it) -> 0
        "  end Main; "
        "end P;";
    BEZEL_EXPECT_EQ(run(src), 0);  // LSan leg (runAotChecked) proves Box's block freed
}

// ===========================================================================
// PRODUCER AXIS — the receiver itself is also destructor-bearing (not just
// trivial-dispose): its own destructor must run exactly once, on top of the
// Conn-result's independent disposal. Encoding: Conn._n*10 + Box._n, read
// AFTER both `using var`s have gone out of scope (a nested `do…end`).
// ===========================================================================
BEZEL_TEST(fresh_owned_receiver_1859, receiver_with_destructor_disposed_once_28_3) {
    const std::string src =
        "module M; "
        "class Conn "
        "  public static _n: int := 0; "
        "  public Id: int; "
        "  public constructor(id: int) Id := id; end constructor; "
        "  destructor _n := _n + 1; end destructor; "
        "end Conn; "
        "class Box "
        "  public static _n: int := 0; "
        "  public method Wrap(owned c: Conn): owned Conn return c; end Wrap; "
        "  destructor _n := _n + 1; end destructor; "
        "end Box; "
        "class P "
        "  public static method Main(): int "
        "    do using var c := new Box().Wrap(new Conn(7)); end; "
        "    return Conn._n * 10 + Box._n; "  // both disposed once -> 1*10+1 = 11
        "  end Main; "
        "end P;";
    BEZEL_EXPECT_EQ(run(src), 11);
}

// An INTERFACE-typed owned return through a fresh receiver — the same
// announced-owned discriminant applies regardless of Class vs Interface kind.
BEZEL_TEST(fresh_owned_receiver_1859, interface_result_receiver_disposed_28_3) {
    const std::string src =
        "module M; "
        "interface IConn "
        "  method Ping(): int; "
        "end IConn; "
        "class Conn : IConn "
        "  public static _n: int := 0; "
        "  public method Ping(): int return 1; end Ping; "
        "  destructor _n := _n + 1; end destructor; "
        "end Conn; "
        "class Box "
        "  public static _n: int := 0; "
        "  public method Wrap(owned c: Conn): owned IConn return c; end Wrap; "
        "  destructor _n := _n + 1; end destructor; "
        "end Box; "
        "class P "
        "  public static method Main(): int "
        "    do using var c := new Box().Wrap(new Conn()); end; "
        "    return Conn._n * 10 + Box._n; "  // 1*10+1 = 11
        "  end Main; "
        "end P;";
    BEZEL_EXPECT_EQ(run(src), 11);
}

// ===========================================================================
// CONTROLS (regression pins):
// ===========================================================================
//
// CRITICAL EXCLUSION — a BORROW-returning method (no `owned`) used on a fresh
// receiver must still NOT free the receiver: the returned class reference IS
// the receiver's own sub-object/borrow (K1.5's ordinary row — `calleeReturns
// Owned` is false there), so disposing the receiver would dangle it. This
// fix adds no new admission for that shape — it stays on the pre-existing,
// already-covered exclusion path. Rather than duplicate that coverage here,
// this file relies on the existing regression pins in
// fresh_owned_receiver_disposal_1039_test.cpp (which this fix's own change
// does not touch): `class_reference_result_not_disposed` and
// `borrow_returning_receiver_not_disposed_1053` — both re-run alongside this
// file's tests as part of this fix's verification (same `fresh_owned_
// receiver` gtest-name substring covers both files).

// A BOUND receiver (`using var b := new Box(); b.Wrap(...)`) must be
// unaffected — the fresh-receiver path only ever fires for an UNBOUND
// receiver expression (isFreshOwnedClassProducer requires the receiver EXPR
// itself to be a NewExpr/CallExpr — a bound IdentifierExpr never matches).
BEZEL_TEST(fresh_owned_receiver_1859, bound_receiver_unaffected_no_double_free_28_2) {
    const std::string src =
        "module M; "
        "class Conn "
        "  public static _n: int := 0; "
        "  public Id: int; "
        "  public constructor(id: int) Id := id; end constructor; "
        "  destructor _n := _n + 1; end destructor; "
        "end Conn; "
        "class Box "
        "  public static _n: int := 0; "
        "  public method Wrap(owned c: Conn): owned Conn return c; end Wrap; "
        "  destructor _n := _n + 1; end destructor; "
        "end Box; "
        "class P "
        "  public static method Main(): int "
        "    do "
        "      using var b := new Box(); "
        "      using var c := b.Wrap(new Conn(7)); "
        "    end; "
        "    return Conn._n * 10 + Box._n; "  // 1*10+1 = 11, each disposed once
        "  end Main; "
        "end P;";
    BEZEL_EXPECT_EQ(run(src), 11);
}
