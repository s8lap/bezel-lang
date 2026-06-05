// Spec-derived tests for Ownership slice O7 — borrow lifetime escape
// through return (§28.3).
//
// A returned borrow's source must outlive the caller's frame. A
// `using var` local is scope-bound — returning a borrow rooted at a
// using-var leaks the source.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

std::string wrap(const std::string& method) {
    return std::string("module M; "
        "class T end T; class View end View; "
        "class Connection "
        "  public method View(): View => null; "
        "end Connection; "
        "class C ") + method + " end C;";
}

} // namespace

BEZEL_TEST(own_escape, return_member_call_on_using_var_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Leak(): View "
        "  using var c := new Connection(); "
        "  return c.View(); "
        "end Leak;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_escape, return_field_access_on_using_var_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "public method LeakField(): View "
        "  using var c := new Connection(); "
        "  return c.Inner; "
        "end LeakField;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_escape, return_member_call_on_param_clean) {
    // Param `c` outlives the caller's frame, so returning c.View() is
    // fine (the caller's `c` source persists past this method's exit).
    auto r = parseAndCheckOwnership(wrap(
        "public method View(c: Connection): View "
        "  return c.View(); "
        "end View;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_escape, return_bare_local_not_borrow_escape) {
    // `return c;` where c is a plain `var` is a discharge (the local's
    // ownership transfers to the caller); not a borrow escape.
    auto r = parseAndCheckOwnership(wrap(
        "public method Take(): Connection "
        "  var c := new Connection(); "
        "  return c; "
        "end Take;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A *value-typed* projection of a `using var` (an int field or an
// int-returning method) is a copy, not a borrow — returning it cannot
// outlive the source, so it is not a borrow-escape (§28.3 governs
// borrows). The reference-typed cases above remain diagnosed.
BEZEL_TEST(own_escape, return_value_field_on_using_var_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class R public V: int := 0; end R; "
        "class C public method Get(): int "
        "  using var r := new R(); return r.V; "
        "end Get; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_escape, return_value_method_on_using_var_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class R public V: int := 0; "
        "  public method Size(): int => V; end R; "
        "class C public method Get(): int "
        "  using var r := new R(); return r.Size(); "
        "end Get; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 (D15 reject-valid regression): a built-in collection's value-typed
// `.Length` projection is `int` (stdlib §3), a copy — not a borrow of the
// collection. Returning it from a `using var` collection is NOT a borrow
// escape. Sema types `Length`/`Count` only on the *call* form, so the bare
// property access reaches the ownership pass untyped; the value-typed
// fallback (returnedExprMayBorrow) must still recognize it as a copy.
BEZEL_TEST(own_escape, return_length_on_using_var_collection_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); a.Add(1); "
        "    return a.Length; "
        "  end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 (D15): `.Count` is the same value-typed `int` projection (stdlib §9).
BEZEL_TEST(own_escape, return_count_on_using_var_collection_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); a.Add(1); "
        "    return a.Count; "
        "  end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 (D15): `.Get(0)` yields the value-typed element (`int` here), a
// copy — sema types the CallExpr, so returning it is not a borrow escape.
BEZEL_TEST(own_escape, return_value_element_get_on_using_var_collection_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); a.Add(7); "
        "    return a.Get(0); "
        "  end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 GUARD (must NOT be weakened by the value-typed fix): returning a
// REFERENCE-typed *field* of a `using var` still aliases into the scope-
// bound source and is rejected as a borrow escape. The value-typed fallback
// recognizes Length/Count/IsEmpty by name only; a class-typed field `.Inner`
// is typed by sema as a borrowable class type, so it is still diagnosed —
// the fix narrows acceptance to value-typed copies only.
BEZEL_TEST(own_escape, return_reference_field_on_using_var_still_diagnosed) {
    auto r = parseAndCheckOwnership(
        "module M; class View end View; "
        "class Connection public Inner: View := null; end Connection; "
        "class C public method Leak(): View "
        "  using var c := new Connection(); "
        "  return c.Inner; "
        "end Leak; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §23.4 / §28.3 (FINDING [6]): reading a VALUE-typed USER-DEFINED indexer
// `c[5]` (element type `int`) of a `using var` local and returning the bound
// result is a value copy, not a borrow — accepted. Sema now types the
// user-indexer IndexExpr (§23.4 result type = the indexer property's declared
// type), so the borrow-vs-copy predicate (§28.3) recognizes the `int` result
// as a copy. The whole-program FINDING-[6] repro: `var v := c[5]; return v;`.
BEZEL_TEST(own_escape, return_value_user_indexer_on_using_var_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public property [i: int]: int "
        "    get => i * 2; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var v := c[5]; "
        "    return v; "
        "  end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §23.4 / §28.3 (FINDING [6]): the direct-return form `return c[5];` of the
// value-typed user-indexer read is likewise a value copy — accepted.
BEZEL_TEST(own_escape, return_value_user_indexer_direct_on_using_var_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public property [i: int]: int "
        "    get => i * 2; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c[5]; "
        "  end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 GUARD (cardinal risk — must NOT be over-accepted by the FINDING-[6]
// fix): a USER-DEFINED indexer whose element type is a CLASS returns a borrow
// of the receiver (§28.3 — a class-typed projection of an owned local aliases
// it). Reading it from a `using var` local and returning the bound result lets
// the borrow outlive its source — still rejected. The fix narrows acceptance
// to VALUE-typed indexer results only.
BEZEL_TEST(own_escape, return_reference_user_indexer_on_using_var_diagnosed) {
    auto r = parseAndCheckOwnership(
        "module M; class R public V: int := 0; end R; "
        "class C "
        "  public _slot: R := null; "
        "  public property [i: int]: R "
        "    get => _slot; "
        "  end; "
        "  public static method Leak(): R "
        "    using var c := new C(); "
        "    var v := c[0]; "
        "    return v; "
        "  end Leak; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.3 / §28.11 GUARD (cardinal risk): a built-in `OwnedArray<R>` slot read
// `owner[0]` (R a class) produces a BORROW of the slot (spec §28.3: "the
// indexer-get produces a borrow"). Reading it from a `using var` owned-array
// local and returning the bound result is still rejected — the FINDING-[6]
// fix must not over-accept this. The element type `R` is class-typed →
// isBorrowableType is true → kept a borrow.
BEZEL_TEST(own_escape, return_owned_array_slot_on_using_var_diagnosed) {
    auto r = parseAndCheckOwnership(
        "module M; class R public V: int := 0; destructor end destructor; end R; "
        "class C public static method Leak(): R "
        "  using var owner := new OwnedArray<R>(); "
        "  owner.Add(move (new R())); "
        "  var v := owner[0]; "
        "  return v; "
        "end Leak; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.3 (D15): a VALUE-typed built-in collection indexer `a[0]` (`Array<int>`,
// element `int`) read from a `using var` local and returned is a value copy —
// accepted. (Sema types this via the built-in-collection IndexExpr path; the
// FINDING-[6] user-indexer branch is distinct and leaves it unaffected.)
BEZEL_TEST(own_escape, return_value_builtin_indexer_on_using_var_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); a.Add(42); "
        "    return a[0]; "
        "  end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (FINDING [6]): the indexer read of a BORROWED PARAM (not an owned
// local) returned is accepted regardless — the param outlives the caller's
// frame. Preserved here as a guard that the fix did not perturb the param
// path.
BEZEL_TEST(own_escape, return_value_user_indexer_on_param_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public property [i: int]: int "
        "    get => i * 2; "
        "  end; "
        "  public static method Use(c: C): int "
        "    return c[5]; "
        "  end Use; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
