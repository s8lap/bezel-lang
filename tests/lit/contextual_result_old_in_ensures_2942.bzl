// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Spec §22.16 (v0.250), issue #3038 (Option B): `result` and `old(E)` are
// contextual keywords valid inside `ensures` clauses, where the spec
// defines them as the method's return value (read as a borrow) and the
// entry-time value of E respectively. #2942 shipped a SAFE LOUD REJECT
// (Option A) pending the binding + capture machinery; #3038 implements
// Option B: `result` is bound to a synthetic local typed as the declared
// return type, and `old(E)` is captured at method entry into a frame
// slot, with the §22.16 ownership restriction (class-typed operands are
// rejected as a borrow across the whole body would be unsound — see the
// sibling REJECT test for that arm).
//
// This lit pin covers the INSIDE-`ensures` accept arms:
//   (1) `result` in an `ensures` clause      → accepted, compiles cleanly
//   (2) `old(E)` in an `ensures` clause      → accepted, compiles cleanly
//
// FAILS-without / PASSES-with: without #3038, every ensures-clause use of
// `result` / `old` falls into the #2942 "not yet implemented in `ensures`
// clauses" diagnostic and compilation fails on the first clause. With
// #3038, both clauses compile cleanly — FileCheck matches nothing
// (i.e. NO diagnostic in the output), which is the ACCEPT shape.
//
// (The outside-`ensures` arm — in body, requires, invariant — is covered
// by `contextual_result_old_outside_ensures_2942.bzl`. The class-typed
// `old(E)` arm — also an inside-`ensures` reject — is covered by
// `class_typed_old_rejected_3038.bzl` in this same revision.)

module M;
class C
  // (1) `result` in an `ensures` clause — accepted; #3038 binds a
  // synthetic local typed as the declared return type, loaded from
  // `ensuresResultSlot_` at the ensures evaluation point.
  public static method Dbl(n: int): int
    ensures(result = n * 2)
    return n * 2;
  end Dbl;

  // (2) `old(E)` in an `ensures` clause — accepted; #3038 captures
  // E at method entry into a frame slot the clause reads at exit.
  public method TouchAndCheck(x: int): int
    ensures(old(x) = 42)
    return 0;
  end TouchAndCheck;
end C;

// NODIAG-NOT: not yet implemented
// NODIAG-NOT: unresolved identifier
// NODIAG-NOT: contextual keyword
