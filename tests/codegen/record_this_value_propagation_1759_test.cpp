// fix #1759 / §9 / §3.2: a record method's `this` propagated as a VALUE.
//
// §9: "A record is a value type. Records have value semantics: assignment
// copies the value." §3.2: "A value type is copied on assignment; each variable
// holds an independent value." Inside a record instance method the receiver
// `this` is lowered to a POINTER to the caller's value struct (a codegen
// convention — spilled so `this.field` reuses ordinary record field access).
// When the WHOLE `this` value flows to a location that expects the record
// STRUCT — assigned to a record-typed slot (`field := this`), returned
// (`return this`), or bound to an inferred local (`var q := this`) — codegen
// must LOAD `*this` (a value-semantics copy) rather than propagate the raw
// 8-byte pointer.
//
// Before the fix three defects (issue #1759):
//   A. `field := this` stored the pointer bits into the record-typed slot, so a
//      later read returned garbage (the report's int-only repro returned 32, not
//      7; with a string field it read a bogus bz_string header → ASan abort).
//   B. `return this` emitted `ret ptr` from a `%record.T`-returning function →
//      "Function return type does not match operand type of return inst" → IR
//      verification failure (a hard compile abort).
//   C. `var q := this` left the inferred local UNTYPED — `this` is deliberately
//      untyped in sema (member access on `this` resolves via the enclosing type
//      directly, not via `this`'s exprType), so seeding an inferred local from a
//      bare `this` produced no type and every later `q.field` folded to
//      `ret i32 0`. Fixed in sema (seed the local with the receiver type) plus
//      codegen (load `*this` into the value slot).
//
// AOT is the correct oracle here: repro B is a front-end IR-verifier abort that
// the shipping bezelc path enforces. All observables fit an 8-bit exit code.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1759 Repro A / §9 / §3.2: a record method assigning the whole `this` into an
// external record-typed slot (`sink.Held := this`) copies the STRUCT, so the
// stashed record reads back its real field value (7), not the pointer bits.
BEZEL_TEST(record_this_value_propagation_1759, field_assign_this_copies_struct) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record P "
        "  public property K: int; "
        "  public method Stash(sink: Sink): int  sink.Held := this; return K; end Stash; "
        "end P; "
        "class Sink "
        "  public property Held: P; "
        "  public constructor() Held := new P(K := 0); end constructor; "
        "end Sink; "
        "class C "
        "  public static method Main(): int "
        "    using var sink := new Sink(); "
        "    var p := new P(K := 7); "
        "    var d := p.Stash(sink); "
        "    return sink.Held.K; "
        "  end Main; "
        "end C;"), 7);
}

// #1759 Repro B / §9 / §3.2: `return this` from a record method returns a
// value-semantics COPY of the struct. Before the fix this failed IR
// verification (`ret ptr` vs a `%record.P` return type). The record carries a
// string field so the returned value must be a real struct to read `.Name`.
//
// `Name` carries a field initializer (fix #4001, landed in this same batch:
// the record no-ctor §7.9 screen now correctly flags a body-only record's
// non-nullable-string auto-property too — `string` is not defaultable,
// ExprTypecheck_Exprs.cpp's `defaultIllFormed` — so a bare `property Name:
// string;` with no constructor is rejected, since the SAME implicit
// memberwise construction this test's `new P(Name := ..., K := 1)` uses also
// admits omitting `Name` entirely, e.g. `new P(K := 1)`, which would leave it
// unset). The initializer is overridden by the named argument at each `new
// P(...)` call site below (verified: a field initializer's default is used
// only when a construction omits the corresponding named argument), so this
// is orthogonal to what the test exercises.
BEZEL_TEST(record_this_value_propagation_1759, return_this_copies_struct) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record P "
        "  public property Name: string := \"\"; "
        "  public property K: int; "
        "  public method Copy(): P  return this; end Copy; "
        "end P; "
        "class C "
        "  static method F(n: int): P "
        "    var p := new P(Name := $\"leak{n}\", K := 1); "
        "    var q := p.Copy(); "
        "    return q; "
        "  end F; "
        "  public static method Main(): int  var r := F(3); return r.Name.Length; end Main; "
        "end C;"), 5);  // "leak3".Length
}

// #1759 Repro C / §9 / §3.2: `var q := this` binds an inferred local to a COPY
// of the record; the local is properly typed as the receiver record, so
// `q.field` reads the real field values (3*10 + 4 = 34), not a folded 0.
BEZEL_TEST(record_this_value_propagation_1759, var_from_this_infers_record_type) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Pt(X: int, Y: int) "
        "  public method Diag(): int  var q := this; return q.X * 10 + q.Y; end Diag; "
        "end Pt; "
        "class C public static method Main(): int "
        "  var p := new Pt(3, 4); "
        "  return p.Diag(); "
        "end Main; end C;"), 34);
}

// #1759 Repro C (class control) / §3.2: the `var q := this` inference gap was
// not record-specific — a class receiver bound the same way also lost its type
// and folded `q.field` to 0. Here `q` aliases the receiver (reference type) and
// reads its field (42). Guards the sema type-seeding for the non-record case.
BEZEL_TEST(record_this_value_propagation_1759, var_from_this_class_receiver) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Box "
        "  public v: int; "
        "  public constructor(n: int) v := n; end; "
        "  public method Diag(): int  var q := this; return q.v; end Diag; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(42); "
        "  return b.Diag(); "
        "end Main; end C;"), 42);
}
