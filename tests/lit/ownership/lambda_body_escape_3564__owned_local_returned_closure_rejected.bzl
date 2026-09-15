// §28.3 "Captured owned bindings" (v0.286), issue #3564: the owned
// scope-bound-local half of the lambda-body traversal hole fix #3546 named.
//
// §28.9 (restating §28.3's rule): "Capturing an *owned* class-typed local does **not** transfer
// ownership into the closure: the capture is a borrow whose source is the
// local's enclosing scope, borrow-state throughout — the closure may not
// assign to, transfer from, or dispose the captured binding, and may not
// outlive that scope (§28.3)." That is the same rule the METHOD-level
// spelling of this program has always been rejected by. Inside a lambda body
// it could not fire:
// the leg resolves the captured symbol through `states_`, and `states_` held
// the METHOD's locals — a lambda body's own `LocalDecl`s were never in it.
//
// fix #3564 gives the driver the lambda body's own scope: `states_` is saved,
// cleared and re-seeded from that body's own owned obligations
// (`localIsOwnedObligation`, the same predicate the method-level dataflow
// uses), so the existing leg answers about the right scope.
//
// `r` is disposed when the lambda invocation leaves the inner `do ... end`,
// before the returned closure is ever called. Measured before the fix:
// compiled rc=0, printed 10, and leaked 56 bytes in 3 allocations under
// LeakSanitizer.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;
import System;
class R
  public method V(): int
    return 5;
  end V;
end R;
class C
  public static method Run(): int
    var mk := lambda(): () -> int => do
      do
        using var r := new R();
        return lambda(): int => r.V();
      end;
      return lambda(): int => 0;
    end;
    var g := mk();
    return g() + g();
  end Run;
  public static method Main(): int
    Console.WriteLine(Run().ToString());
    return 0;
  end Main;
end C;

// The §28.3 diagnostic is the method-level one, verbatim.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned closure captures `r` whose source is disposed at scope end
// CHECK-SAME: (§28.3 closure-capture escape
