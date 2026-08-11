// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec-derived regression test for issue #2838.
//
// §12.3 (`base.Member`) / §12.5 (Abstract classes and members) / §23
// (Properties), v0.240.
//
// §12.3 states, verbatim:
//
//     "Inside a derived member, `base.Name(...)` calls the base class's
//      implementation, bypassing virtual dispatch. `base` is legal only
//      inside instance members of a derived class."
//
// and §12.5: "`abstract` on a member declares it without a body; every
// concrete subclass must override it." The named-method form already
// rejects this (#874); the indexer form already rejects this (#2832);
// the named-property form was the remaining shape where `base.P` silently
// produced 0 — codegen's `base`-bypass branch (lowerVirtualPropertyGet
// / -Set's `baseQualified` decline, paired with the static accessor-body
// path) found no body on the abstract base and zero-init'd. Reject here.

module M;
abstract class B public abstract property P: int; end B;
class D: B
  public override property P: int => 7;
  public method ViaBaseProp(): int
    return base.P;
  end ViaBaseProp;
end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot access an abstract base property `P` via `base` — it has no implementation
// CHECK-SAME: (§12.3/§12.5)
