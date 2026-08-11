// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §22.16 (v0.240), issue #2941: a class `invariant(E);` member is
// parsed, purity-checked, AND EVALUATED at the construction-exit /
// public-method-entry / public-method-exit boundaries §22.16 fixes. The
// codegen-side wiring (src/codegen/LlvmBackend_Setup.cpp
// `emitInvariantCheck`, `armEnsuresEpilogue`'s invariantClass parameter,
// and `lowerNew`'s post-construction call site) is what makes this work.
//
// This lit pin covers the SEMANTIC half that produces a COMPILE-TIME
// diagnostic: an invariant predicate whose purity check rejects it. The
// spec requires the predicate to be pure (no stores, no throws, no awaits,
// no owned-consuming positions — §22.16 "Clause typing: bool, pure,
// borrow-only"); a predicate that fails purity cannot be evaluated at
// construction exit or public boundaries. The checker is `PureBodyWalker`
// in src/sema/PureChecker.cpp `checkInvariantExpr` (called from
// `checkClassInvariants` per-class before the member walk), which is
// §22.16's clause-mode walker (throw/await rejected, pure-body rules
// expression-wise).
//
// FAILS-without / PASSES-with: without the sema-side purity check on the
// invariant predicate, an `invariant(Impure())` would compile silently
// and the runtime would either crash (the predicate has effects that the
// boundary can't model) or silently never fire. With the check, bezelc
// emits the located Error and exits non-zero; `%not` inverts that to 0
// and FileCheck matches, so the test passes.
//
// Three axes — each tests a distinct purity-rule violation reachable via
// the invariant predicate's expression:
//   (a) a call to a non-pure method (one that allocates / has effects) —
//       §22.15 "Pure-body rules", per-call. An invariant that calls such
//       a method could fault at every public-method exit.
//   (b) a CALL TO A PURE METHOD WHOSE BODY IS IMPURE — the static
//       purity rule admits the call (the callee is `pure`), but the
//       body fails the §22.15 closed list when walked. The pure body's
//       own `using var` is the load-bearing impurity. An invariant that
//       admitted such a call would fault on the impure body's own
//       boundary check.
//   (c) a mutating method invocation — a non-pure int-returning method
//       that mutates `this`. An invariant that admits a mutator would
//       break the borrow-only guarantee.
//
// CHECK-DAG so the order of diagnostic emission doesn't matter — the
// diagnostics arrive in the order the predicates are walked, which is
// declaration order within the invariant group, but interleaves with
// the body diagnostics on pure-typed members.

module M;
class C
  public x: int := 0;
  public method AllocList(): int
    using var t := new Array<int>();
    t.Add(1);
    return t.Count;
  end AllocList;
  invariant(AllocList() > 0);
// CHECK-DAG: AllocList
// CHECK-DAG: not pure-declared
  public pure method PureButImpureBody(): int
    using var t := new Array<int>();
    return t.Count;
  end PureButImpureBody;
  invariant(PureButImpureBody() > 0);
// CHECK-DAG: `using var` is not admitted
  public method SetX(v: int): int x := v; return x; end SetX;
  invariant(SetX(5) > 0);
// CHECK-DAG: SetX
// CHECK-DAG: not pure-declared
  public constructor() x := 0; end constructor;
end C;
