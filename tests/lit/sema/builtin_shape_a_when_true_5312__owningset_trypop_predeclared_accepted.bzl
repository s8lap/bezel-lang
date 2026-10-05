// Issue #5312 / stdlib §1 (v0.313) row (a): `OwningSet<T>.TryPop(query,
// out removed: T?)` carries the `when true` postcondition, so the pre-declared
// recipient is narrowed non-null in the controlled branch and the member read
// `r.X` (§7.4) is ACCEPTED without `!`. Sema-level only (compile, no run).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class R public X: int; public constructor(x: int) X := x; end constructor; destructor end destructor; public override method Equals(other: Object?): bool return false; end Equals; public override method GetHashCode(): int => X; end R; class C public static method Main(): int using var s := new OwningSet<R>(); if s.TryPop(new R(1), out var r) then r?.Dispose(); return r.X; end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
