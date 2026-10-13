// fix #3044 (§17.11, v0.242) OVER-REJECTION GUARD, re-pointed for #5820
// (§17.11 v0.338, generic-body list bullet 1). The original row instantiated
// an unconstrained `default(T)` at `string`; v0.338 rejects that at the
// instantiation site ("For unconstrained `T`, … the rule below rejects at the
// instantiation site every substitution that is not defaultable — a
// non-nullable reference type (a class, an interface, `Object`, `string`, …)").
// The row now substitutes `string?`, which is defaultable (its zero is `null`)
// and must stay accepted. The `string` rejection is pinned in the #5820 tests
// (tests/sema/generic_record_default_subst_3986_test.cpp).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

static class U
  public static method Z<T>(): int var t := default(T); return 7; end Z;
end U;

class C public static method Main(): int return U.Z<string?>(); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
