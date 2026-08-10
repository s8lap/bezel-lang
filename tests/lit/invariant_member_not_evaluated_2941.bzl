// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §22.16 (v0.240), issue #2941 (INTERIM): a class `invariant(E);` member
// is parsed, purity-tagged, and carried in the artifact interface (§2.8.2) but
// is NOT evaluated at the construction-exit / public-entry / public-exit
// boundaries §22.16 fixes — full invariant enforcement is contracts feature
// build-out (the §22.16 / #2053 area) and is out of scope for this interim.
// The safe, loud half: every SOURCE-defined invariant member is a located
// compile Error, so a programmer cannot be silently misled into believing the
// predicate is enforced.
//
// FAILS-without / PASSES-with: without the interim, bezelc accepted every
// invariant silently (exit 0, no diagnostic) — `%not` then fails the RUN line
// and FileCheck matches nothing, so the test fails. With the interim, bezelc
// emits the located Error and exits non-zero; `%not` inverts that to 0 and
// FileCheck matches, so the test passes.
//
// Six axes — each was a silent no-op before (the predicate was never
// name-resolved, type-checked, OR evaluated, since checkInvariantExpr was
// unreached dead code):
//   (a) a valid pure boolean predicate — still unenforced, so still an Error;
//   (b) an unresolved identifier — was silently accepted (no name resolution);
//   (c) a non-bool (string) expression — was silently accepted (no type check);
//   (d) a `true`/`false` literal — constant folding would prove it, but
//       presently a non-`static` instance invariant is checked at runtime;
//       the interim rejects it before any tier choice is made (the runtime
//       path is not built out — see #2053 follow-up);
//   (e) an arithmetic expression — was silently accepted;
//   (f) a method call on a value-typed receiver — was silently accepted.
//
// Note: cell (c) "calling a mutating method" is NOT a separate cell here
// because the rule is "every invariant is rejected" — the predicate is never
// purity-checked or evaluated, so its *content* (whether pure, impure, bool,
// string, valid, invalid) is irrelevant to the interim's behavior. The test
// pins the rejection across six distinct SHAPES so a future fix that
// begins to evaluate invariants (and would therefore accept some of these)
// cannot accidentally land without updating this test — the rejected/error
// contract is the documented interim contract.
module M;
class C
  public x: int := 0;
  invariant(x > 0);
// CHECK: :[[@LINE-1]]:{{[0-9]+}}: `invariant` member is parsed but not evaluated in this compiler revision (§22.16)
// CHECK-SAME: class invariants are not checked at construction exit or public boundaries, so this predicate is unenforced
  invariant(no_such_field);
// CHECK: :[[@LINE-1]]:{{[0-9]+}}: `invariant` member is parsed but not evaluated in this compiler revision (§22.16)
// CHECK-SAME: class invariants are not checked at construction exit or public boundaries, so this predicate is unenforced
  invariant("not a bool");
// CHECK: :[[@LINE-1]]:{{[0-9]+}}: `invariant` member is parsed but not evaluated in this compiler revision (§22.16)
// CHECK-SAME: class invariants are not checked at construction exit or public boundaries, so this predicate is unenforced
  invariant(true);
// CHECK: :[[@LINE-1]]:{{[0-9]+}}: `invariant` member is parsed but not evaluated in this compiler revision (§22.16)
// CHECK-SAME: class invariants are not checked at construction exit or public boundaries, so this predicate is unenforced
  invariant(x + 1 > 0);
// CHECK: :[[@LINE-1]]:{{[0-9]+}}: `invariant` member is parsed but not evaluated in this compiler revision (§22.16)
// CHECK-SAME: class invariants are not checked at construction exit or public boundaries, so this predicate is unenforced
  invariant(x.ToString().Length > 0);
// CHECK: :[[@LINE-1]]:{{[0-9]+}}: `invariant` member is parsed but not evaluated in this compiler revision (§22.16)
// CHECK-SAME: class invariants are not checked at construction exit or public boundaries, so this predicate is unenforced
  public constructor() x := 5; end constructor;
end C;
