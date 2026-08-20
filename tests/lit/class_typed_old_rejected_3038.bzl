// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §22.16 (v0.250), issue #3038: the `old(E)` capture taxonomy —
// class- and interface-typed operands are captured AS A FRAME-RESIDENT
// BORROW, which would hold a borrow across the entire method body
// (§22.16 "old(E) — class- and interface-typed operands are captured
// as a frame-resident borrow"). The spec explicitly rejects this with
// a diagnostic naming the copyable-projection remedy. This lit pin
// verifies that the class-typed `old(E)` arm of the new §22.16
// implementation is rejected with the spec-cited diagnostic.
//
// FAILS-without / PASSES-with: without #3038, every `old(E)` is
// rejected by #2942 with the "not yet implemented in `ensures` clauses"
// diagnostic (the inside-ensures one, not the class-typed one). The
// class-typed `old` form did not have its own diagnostic — it inherited
// the generic "not yet implemented" message. With #3038, the class-
// typed operand is admitted and the ownership restriction fires its
// OWN diagnostic, which FileCheck matches below.
//
// The sibling inside-`ensures` ACCEPT test (for value-typed `old` and
// bare `result`) is `contextual_result_old_in_ensures_2942.bzl`.

module M;
class D
  public v: int := 0;
  public constructor(v: int) this.v := v; end constructor;
end D;

class C
  // class-typed `old(E)` — rejected per §22.16 ownership restriction.
  public method ClassOld(d: D): int
    ensures(old(d).v = 42)
    return 0;
  end ClassOld;
end C;

// CHECK: would hold a borrow across the entire method body
// CHECK: copyable projection
