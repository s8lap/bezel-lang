// fix #835 / §28.8: the owned-argument transfer check (a borrow may not flow
// into an `owned` constructor/method parameter) was bypassed when the producer
// was NESTED inside another `new`/call — `new Outer(new Box(_c))` left the inner
// `new Box(_c)`'s argument `_c` only `checkRead`, so a borrow (an owned field
// read without `move`, a borrowing parameter) flowing into the inner `owned`
// parameter was silently accepted → double-free / SIGSEGV. handleDischarge now
// recurses into a nested `new`/call argument so its owned arguments are checked,
// exactly as a top-level producer's are.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// Cell (disposable), Box(owned Cell), Outer(owned Box) — the bug07 shape.
std::string withTypes(const std::string& cls) {
    return std::string(
        "module M; "
        "class Cell public val: int; "
        "  public constructor(v: int) val := v; end constructor; "
        "  destructor end destructor; end Cell; "
        "class Box private owned _c: Cell; "
        "  public constructor(owned v: Cell) _c := v; end constructor; "
        "  destructor end destructor; end Box; "
        "class Outer private owned _b: Box; "
        "  public constructor(owned v: Box) _b := v; end constructor; "
        "  destructor end destructor; end Outer; ") + cls;
}

bool hasOwnedTransferError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("a borrow cannot be passed where an `owned`") !=
            std::string::npos)
            return true;
    return false;
}

}  // namespace

// §28.8: a borrow (owned field read, no `move`) flowing into the INNER `new`'s
// owned parameter — nested inside an outer `new` — is rejected (was accepted →
// double-free). The bug07 repro.
BEZEL_TEST(nested_new_owned_arg_835, nested_new_owned_field_borrow_rejected) {
    auto r = parseAndCheckOwnership(withTypes(
        "class Source private owned _c: Cell; "
        "  public constructor(owned v: Cell) _c := v; end constructor; "
        "  public method Bad(): owned Outer "
        "    return new Outer(new Box(_c)); "
        "  end Bad; destructor end destructor; end Source;"));
    BEZEL_EXPECT_TRUE(hasOwnedTransferError(r));
}

// A borrowing PARAMETER flowing into the nested inner owned param is rejected.
BEZEL_TEST(nested_new_owned_arg_835, nested_new_borrow_param_rejected) {
    auto r = parseAndCheckOwnership(withTypes(
        "class C public static method Bad(c: Cell): owned Outer "
        "  return new Outer(new Box(c)); "
        "end Bad; end C;"));
    BEZEL_EXPECT_TRUE(hasOwnedTransferError(r));
}

// The CALL form (nested `new` inside a method call argument) is also checked.
BEZEL_TEST(nested_new_owned_arg_835, call_nested_new_borrow_rejected) {
    auto r = parseAndCheckOwnership(withTypes(
        "class C "
        "  public static method Take(owned o: Outer) o.Dispose(); end Take; "
        "  public static method Bad(c: Cell) "
        "    C.Take(new Outer(new Box(c))); "
        "  end Bad; end C;"));
    BEZEL_EXPECT_TRUE(hasOwnedTransferError(r));
}

// Valid: a fresh `new Cell(...)` (an owned producer) nested through the chain is
// accepted — the transfer is real, not a borrow.
BEZEL_TEST(nested_new_owned_arg_835, nested_new_fresh_producer_accepted) {
    auto r = parseAndCheckOwnership(withTypes(
        "class C public static method Mk(): owned Outer "
        "  return new Outer(new Box(new Cell(5))); "
        "end Mk; end C;"));
    BEZEL_EXPECT_FALSE(hasOwnedTransferError(r));
}

// Valid: `move`-ing an owned local through the nested producer is accepted.
BEZEL_TEST(nested_new_owned_arg_835, nested_new_move_local_accepted) {
    auto r = parseAndCheckOwnership(withTypes(
        "class C public static method Mk(): owned Outer "
        "  var c := new Cell(5); "
        "  return new Outer(new Box(move c)); "
        "end Mk; end C;"));
    BEZEL_EXPECT_FALSE(hasOwnedTransferError(r));
}
