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
// Three axes — each was a silent no-op before (the predicate was never
// name-resolved, type-checked, OR evaluated, since checkInvariantExpr was
// unreached dead code):
//   (1) a valid pure boolean predicate — still unenforced, so still an Error;
//   (2) an unresolved identifier — was silently accepted (no name resolution);
//   (3) a non-bool (string) expression — was silently accepted (no type check).
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
  public constructor() x := 5; end constructor;
end C;
