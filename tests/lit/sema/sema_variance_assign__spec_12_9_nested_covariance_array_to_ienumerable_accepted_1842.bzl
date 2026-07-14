// Ported from issue #1842's no-over-reject requirement.
//
// §12.9 (v0.196): "Position is composed across nested type constructors." The
// covariant conversion `Array<Array<Dog>>` → `IEnumerable<IEnumerable<Dog>>` is
// LEGAL: the element conversion `Array<Dog>` → `IEnumerable<Dog>` is a covariant
// reference conversion (Array implements IEnumerable) with an identical inner
// argument (`Dog`). It must be accepted.
//
// Fails-without-fix (the sibling wrong-reject of #1842): the pre-fix per-argument
// covariant check (`typeArgRefSubtype`) neither walked the builtin interface
// hierarchy (Array→IEnumerable) nor recursed into nested arguments, so it wrongly
// REJECTED this legitimate nested covariance. Passes-with-fix: recursion into the
// nested arguments composes the two covariant steps.
//
// Compile-only ACCEPT control: the runtime iteration of a NESTED covariant
// IEnumerable view hits a separate codegen defect (#1850), so this pins only
// the sema decision — bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Animal end Animal; class Dog: Animal end Dog; class C public method M(a: Array<Array<Dog>>) var x: IEnumerable<IEnumerable<Dog>> := a; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
