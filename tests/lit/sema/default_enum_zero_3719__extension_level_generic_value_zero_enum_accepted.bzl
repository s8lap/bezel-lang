// fix #3719 (§17.11 / §3.6 rule 2 / §14.2 / §26.4, v0.294) OVER-REJECTION
// GUARD: the same EXTENSION-level `where T: value` generic, instantiated
// with an enum that DOES declare a zero-valued constant. `default(Level)` is
// `Zero`, so §17.11's guarantee holds and the instantiation stands. Pairs
// with
// default_enum_zero_3719__extension_level_generic_value_instantiation_rejected.bzl
// — same program, the only difference is `Zero := 0`.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

enum Level Zero := 0, Low := 1, Mid := 2; end Level;

class Box<K>
  public V: K?;
end Box;

extension Box<T> where T: value
  public method IsZero(): bool
    var t := default(T);
    return t = t;
  end IsZero;
end;

class C
  public static method Main(): int
    var b := new Box<Level>();
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
