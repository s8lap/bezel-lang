// Ownership sema — issue #5624 (§28.3, v0.315): a deferred-activation
// frame returned from a method whose body creates an owned local and passes
// it to the iterator carries a borrow of the local — the local is
// auto-dropped at the method's scope end, so the borrow dangles.
//
// Two bugs in `collectReturnBorrowRoots` (Ownership_Checks.cpp:4042):
//
// Bug A — deferred call args not traced: for a CallExpr it traces only the
// callee chain (method reference → class), never examining the call's plain
// reference-typed arguments. When a deferred-activation callee (iterator
// factory, async iterator, lazy query, Task-family) is returned, the frame it
// returns holds borrows of its plain reference-typed arguments. Fix: when
// `isDeferredActivationCallResult` is true, also trace the call arguments.
//
// Bug B — isBorrowableType misses type-argument resolution: ctx_.exprType
// may resolve to the type argument (e.g. Item in IEnumerable<Item>) rather
// than the declared return type. Fix: fall back to isDeferredActivationCallResult
// directly in returnedExprMayBorrow.
//
// Shapes 1 and 2 (concrete + generic) are covered by the fixes.
// Shape 3 (generic + function-typed argument + using var) requires tracing
// through a function call's arguments to find the underlying scope-bound local
// — a more complex analysis that is out of scope for this fix.
//
// Control — forwarded parameter (must be accepted):
//   `Wrap2<T>(x: T): IEnumerable<T> => Once<T>(x);` — `x` is a
//   parameter-sourced argument; the borrow is valid at every caller's scope.

#include <string>

#include "support/ownership_support.hpp"  // parseAndCheckOwnership / hasErrorMessage
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// A class element type — borrow-bearing, disposable.
const char* kItem =
    "class Item "
    "  public static ctorCount: int := 0; "
    "  public static dtorCount: int := 0; "
    "  public constructor() Item.ctorCount := Item.ctorCount + 1; end constructor; "
    "  destructor Item.dtorCount := Item.dtorCount + 1; end destructor; "
    "end Item; ";

}  // namespace

// ===========================================================================
// REJECT shapes — §28.3 borrow-lifetime rule
// ===========================================================================

// Shape 1 (concrete): owned local + iterator factory call = dangling borrow.
// §28.3: a borrow whose source is a scope-bound owned local may not be
// returned; the source is disposed before control returns to the caller.
BEZEL_TEST(deferred_activation_frame_borrow_escape_5624,
           reject_owned_local_arg_to_iterator_concrete) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kItem +
        "class C "
        "  public static method OnceItem(x: Item): IEnumerable<Item> "
        "    yield return x; "
        "  end OnceItem; "
        "  public static method FreshConcrete(): IEnumerable<Item> "
        "    var t := new Item(); "
        "    return OnceItem(t); "   // frame borrows `t`; `t` auto-dropped at exit
        "  end FreshConcrete; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The diagnostic must name `t` as the escaping borrow source.
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "borrow returned from `t`"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "auto-dropped at scope end"));
}

// Shape 2 (generic): same pattern with a generic iterator and a type
// parameter. §28.3 counts a type-parameter-typed argument at every
// substitution its constraints admit (kind 4).
BEZEL_TEST(deferred_activation_frame_borrow_escape_5624,
           reject_owned_local_arg_to_iterator_generic) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kItem +
        "class C "
        "  public static method Once<T>(x: T): IEnumerable<T> "
        "    yield return x; "
        "  end Once; "
        "  public static method Fresh<T>(): IEnumerable<T> where T: new() "
        "    var t := new T(); "
        "    return Once<T>(t); "   // frame borrows `t`; `t` auto-dropped at exit
        "  end Fresh; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "borrow returned from `t`"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "auto-dropped at scope end"));
}

// ===========================================================================
// ACCEPT control — forwarded parameter (the issue explicitly exempts this)
// ===========================================================================

// A forwarded type-parameter argument stays admitted: it is parameter-sourced,
// and the borrow's lifetime is established by the caller, not by the callee's
// local scope.
BEZEL_TEST(deferred_activation_frame_borrow_escape_5624,
           accept_forwarded_param_to_iterator_clean) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kItem +
        "class C "
        "  public static method Once<T>(x: T): IEnumerable<T> "
        "    yield return x; "
        "  end Once; "
        "  public static method Wrap2<T>(x: T): IEnumerable<T> "
        "    return Once<T>(x); "
        "  end Wrap2; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
