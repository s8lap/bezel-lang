// fix #3044 (§17.11 / §3.6 rule 2, v0.242) OVER-REJECTION GUARD: the same
// `default(T)`-under-`where T: value` generic, instantiated with an enum that
// DOES declare a zero-valued constant. `default(Level)` is `Off`, so §17.11's
// "`default(T)` is always non-null under `value`" guarantee holds and the
// instantiation stands. Pairs with
// default_enum_zero_3044__generic_value_instantiation_rejected.bzl — same
// program, the only difference is `Off := 0`.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

enum Level Off := 0, Low := 1, High := 2; end Level;

static class U
  public static method Z<T>(): T where T: value var t := default(T); return t; end Z;
end U;

class C public static method Main(): int var l := U.Z<Level>(); return int(l); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
