// §19.6 + §7.10 (v0.196) / #1816 (s7/s8): the §7.10 `?`-idempotence pre-image
// split undetermined-leaf DIAGNOSTIC. `L<Y> : B<Y?>` matched at scrutinee
// `B<int?>`: the occurrence `Y?` matched against `int?` admits BOTH `Y := int`
// and `Y := int?` (`(int?)? ≡ int?`, §7.10) — two inhabiting instantiations, so
// leaf coverage is statically unattainable even with the `var l: L<int>` arm
// (that names one instantiation, not the leaf). §19.6 v0.196 requires the
// warning to NAME the leaf and its undetermined parameter and to prescribe BOTH
// remedies: (a) a `_` arm (or an unguarded untyped `var`), or (b) restructuring
// to the determined shape — mentioning every leaf parameter in the base clause
// and, for a `?`-split, constraining the parameter non-nullable. This pins the
// exact wording (formatMissingGenericLeaf, s8).
//
// The program is LEGAL (non-exhaustiveness is a WARNING in expression position,
// not an error, §19.6): `bezelc -c` exits zero but emits the diagnostic.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; sealed abstract class B<T> end B; sealed class L<Y> : B<Y?> end L; sealed class R : B<int?> end R; class Calc public method M(b: B<int?>): int var a := (case b of var r: R => 1, var l: L<int> => 2); return a; end M; end Calc;

// CHECK: non-exhaustive
// CHECK-SAME: closed hierarchy `B`
// CHECK-SAME: L (undetermined parameter `Y`)
// CHECK-SAME: statically unattainable
// CHECK-SAME: add a `_` arm
// CHECK-SAME: unguarded untyped `var`
// CHECK-SAME: mentioning every leaf parameter in the base clause
// CHECK-SAME: `?`-split parameter, constraining it non-nullable
