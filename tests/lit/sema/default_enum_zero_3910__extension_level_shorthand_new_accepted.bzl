// fix #3910 (§17.11 / §3.6 rule 2 / §17.7 / §26.4, v0.315): OVER-REJECTION
// GUARD for the extension-level shorthand `new()` fix. Same program as the
// rejected test beside this file, but `Level` declares `Zero := 0`, so
// `default(Level)` is well-defined and the shorthand `new()` at the local
// declaration is accepted.
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
    var b: Box<Level> := new();
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
