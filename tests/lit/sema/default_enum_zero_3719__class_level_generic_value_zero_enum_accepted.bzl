// fix #3719 (§17.11 / §3.6 rule 2 / §14.2, v0.294) OVER-REJECTION GUARD: the
// same CLASS-level `where K: value` generic, instantiated with an enum that
// DOES declare a zero-valued constant. `default(Level)` is `Zero`, so
// §17.11's "`default(T)` is always non-null under `value`" guarantee holds
// and the instantiation stands. Pairs with
// default_enum_zero_3719__class_level_generic_value_instantiation_rejected.bzl
// — same program, the only difference is `Zero := 0`.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

enum Level Zero := 0, Low := 1, Mid := 2; end Level;

class Box<K> where K: value
  public method Get(): K
    var t := default(K);
    return t;
  end Get;
end Box;

class C
  public static method Main(): int
    var b := new Box<Level>();
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
