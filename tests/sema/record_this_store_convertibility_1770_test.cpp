// Spec-derived tests for #1770 (soundness) / §5.1 / §9 / §22: an assignment
// `slot := this` must run the §5.1 implicit-convertibility check on the bare
// `this` receiver, exactly as it does for a named-variable right-hand side.
//
// The bug (#1770 / BAD_ACCEPT): a bare `this` is left deliberately UNTYPED in
// sema (member access on `this` resolves via the enclosing type directly, not
// via `this`'s exprType — see the #1759 note in visitExpr). So at the direct
// assignment site `ctx_.exprType(*as->value)` was null for `this`, and
// `checkAssignable`'s `if (!from) return` guard silently ACCEPTED
// `box.Slot := this` for ANY target type. A record method storing its `this`
// (record type A) into a slot typed as an unrelated record B therefore
// compiled and ran on garbage memory (the receiver POINTER bits reinterpreted
// as B's field), an observed nonsense exit code. Writing the SAME store through
// a named variable (`var q := this; box.Slot := q;`) was correctly rejected —
// the #1759 fix seeds `q`'s type during local inference, but that fix does NOT
// cover the direct `slot := this` path (distinct from #1759, which is the
// `var q := this` local-binding path).
//
// The fix (src/sema/ExprTypecheck.cpp, AssignmentStmt handler): when the plain
// `:=` right-hand side is a bare `this` whose exprType is null, seed `from`
// with the current receiver type (a Named ref keyed on currentType_ — the same
// seed the #1759 local path uses) so the §5.1 convertibility gate runs. A
// same-type store, an upcast to a base class, and an upcast to an interface the
// receiver implements (§8.6 record boxing) are all still accepted; a store into
// a slot of an unrelated record type is now rejected — matching the
// named-variable behavior exactly.
//
// §5.1 (implicit conversions) is the convertibility rule; §9/§3.2 make a record
// a value type (so `slot := this` on a record is a value COPY, the store the
// bug corrupted); §22 defines the method receiver `this`. AOT is the oracle
// that surfaced the garbage the JIT masked (JIT->AOT migration; the reject
// itself is a pure sema check both paths run).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
// True iff the program was rejected at compile time with a §5.1
// "not implicitly convertible" error (the exact #1770 gate).
bool rejectedNotConvertible(const std::string& src) {
    auto r = runAot(src);
    if (r.ran) return false;  // it compiled + ran — the bug
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            return true;
    }
    return false;
}
}  // namespace

// #1770 / §5.1 / §9 / §22 — THE BUG: a record method storing its `this` (type
// A) into a slot typed as an UNRELATED record B is a §5.1 error. Pre-fix this
// compiled and ran on garbage (the receiver pointer bits read back as B.J);
// post-fix it is rejected at compile time.
BEZEL_TEST(record_this_store_convertibility_1770,
           mismatched_record_this_store_rejected_spec_5_1_issue_1770) {
    BEZEL_EXPECT_TRUE(rejectedNotConvertible(
        "module M; "
        "record A "
        "  public property K: int; "
        "  public method Stash(box: Box): int  box.Slot := this; return K; end Stash; "
        "end A; "
        "record B  public property J: int; end B; "
        "class Box "
        "  public property Slot: B; "
        "  public constructor() Slot := new B(J := 0); end constructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var box := new Box(); "
        "  var a := new A(K := 7); "
        "  var d := a.Stash(box); "
        "  return box.Slot.J; "
        "end Main; end C;"));
}

// #1770 control / §5.1: the SAME store written through a NAMED variable was
// already rejected (the #1759 local-type seeding). This pins the parity the
// bug report cites — `this` and `var q := this` must now behave identically.
BEZEL_TEST(record_this_store_convertibility_1770,
           mismatched_named_var_store_still_rejected_spec_5_1_issue_1770) {
    BEZEL_EXPECT_TRUE(rejectedNotConvertible(
        "module M; "
        "record A "
        "  public property K: int; "
        "  public method Stash(box: Box): int  var q := this; box.Slot := q; return K; end Stash; "
        "end A; "
        "record B  public property J: int; end B; "
        "class Box "
        "  public property Slot: B; "
        "  public constructor() Slot := new B(J := 0); end constructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var box := new Box(); "
        "  var a := new A(K := 7); "
        "  var d := a.Stash(box); "
        "  return box.Slot.J; "
        "end Main; end C;"));
}

// #1770 regression / §9 / §3.2: a MATCHING-type `slot := this` (both `P`) must
// still compile and store a value COPY of the record, so the stashed value
// reads back its real field (7), not the pointer bits. (This is the #1759
// Repro A shape; the fix must not break it.)
BEZEL_TEST(record_this_store_convertibility_1770,
           matching_record_this_store_still_accepted_spec_9_issue_1770) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record P "
        "  public property K: int; "
        "  public method Stash(sink: Sink): int  sink.Held := this; return K; end Stash; "
        "end P; "
        "class Sink "
        "  public property Held: P; "
        "  public constructor() Held := new P(K := 0); end constructor; "
        "end Sink; "
        "class C public static method Main(): int "
        "  using var sink := new Sink(); "
        "  var p := new P(K := 7); "
        "  var d := p.Stash(sink); "
        "  return sink.Held.K; "
        "end Main; end C;"), 7);
}

// #1770 regression / §5.1 / §8.6: an UPCAST store — a record `this` into a slot
// of an interface the record implements — is a valid boxing conversion (§8.6:
// "a record value boxed at assignment") and must still compile. The seeded
// receiver type flows through checkAssignable's subtype path, which accepts it.
//
// `Box` carries an explicit constructor assigning `Cur` (fix #4012, landed in
// this same batch: the class no-ctor §7.9 screen now also covers a
// non-nullable auto-property's backing storage, not just plain fields — a
// bare `public property Cur: IShape;` with no initializer and no constructor
// is correctly rejected at DECLARATION regardless of whether `Main` ever
// constructs a `Box`). Orthogonal to what this test actually exercises (the
// §5.1/§8.6 upcast convertibility of `box.Cur := this`).
BEZEL_TEST(record_this_store_convertibility_1770,
           record_this_into_implemented_interface_slot_accepted_spec_8_6_issue_1770) {
    auto r = runAot(
        "module M; "
        "interface IShape  method Area(): int; end IShape; "
        "record Sq : IShape "
        "  public property S: int; "
        "  public method Area(): int  return S * S; end Area; "
        "  public method Put(box: Box)  box.Cur := this; end Put; "
        "end Sq; "
        "class Box  public property Cur: IShape; "
        "  public constructor(c: IShape) Cur := c; end constructor; "
        "end Box; "
        "class C public static method Main(): int  return 0; end Main; end C;");
    // The store must NOT be rejected as non-convertible (it is a valid §8.6
    // upcast); the program compiles and runs.
    BEZEL_EXPECT_TRUE(r.ran);
}

// #1770 regression / §5.1 / §12: an UPCAST store — a derived-class `this` into
// a slot of its base class — is a valid reference upcast and must still
// compile. Guards that the fix accepts a class-subtype `this`, not only the
// exact receiver type.
//
// `Box` again carries an assigning constructor for `Cur` — the same fix #4012
// interaction as the test above.
BEZEL_TEST(record_this_store_convertibility_1770,
           derived_class_this_into_base_slot_accepted_spec_12_issue_1770) {
    auto r = runAot(
        "module M; "
        "class Base "
        "  public property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Base; "
        "class Derived : Base "
        "  public constructor(n: int) base(n); end constructor; "
        "  public method Put(box: Box)  box.Cur := this; end Put; "
        "end Derived; "
        "class Box  public property Cur: Base; "
        "  public constructor(c: Base) Cur := c; end constructor; "
        "end Box; "
        "class C public static method Main(): int  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
}
