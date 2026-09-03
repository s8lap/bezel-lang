// §28.3 "Borrow lifetime rules" / §28.9 "Runtime-managed types" (v0.286),
// issue #3727 — the OWNED-LOCAL sibling of #3707, on the assignment channel.
//
// §28.9: "Capturing an *owned* class-typed local does **not** transfer
// ownership into the closure: the capture is a borrow whose source is the
// local's enclosing scope ... and may not outlive that scope (§28.3)."
// §28.3 *Borrow lifetime rules*: "A borrow may not be assigned to a local
// whose scope strictly encloses the borrow's source's scope."
//
// #3565 drove the whole assignment channel over lambda bodies, and #3707
// closed its catch-binding residual — but both resolve the SOURCE through
// `states_`, which `checkOneLambdaBodyEscapes` clears and re-seeds from the
// checked lambda body ALONE. An owned local declared in the frame that
// ENCLOSES that body therefore resolved to no borrow source at all, and
// `checkBorrowStoreEscape` returned before any depth comparison — however the
// target was declared. `isCatchBorrow` needs no seed (a scope-free sema flag),
// which is why #3707's branch could not be widened to cover this one.
//
// Measured on the parent (28e5b15, which contains #3707): compiles rc=0 with
// NO diagnostic, runs rc=139 (SIGSEGV); under `--sanitize=address`,
// "heap-use-after-free ... in bz$M.C$Run", freed by `bz$lam$1$envdtor`. `r` is
// disposed at the end of the inner `do ... end`, and `f` — declared at method
// scope, outside it — is called twice afterwards through the stored closure.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;
import System;
class R
  private v: int := 5;
  public method V(): int
    return this.v;
  end V;
end R;
class C
  public static method Run(): int
    var f := lambda(): int => 0;
    var total := 0;
    do
      using var r := new R();
      var mk := lambda(): int => do
        f := lambda(): int => r.V();
        return 0;
      end;
      total := mk();
    end;
    return total + f() + f();
  end Run;
  public static method Main(): int
    Console.WriteLine(Run().ToString());
    return 0;
  end Main;
end C;

// The diagnostic is the METHOD-level #861 one, unchanged — the same check,
// reached through a frame it could not resolve before, not a second spelling.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: closure capturing `r` is assigned to `f`, which outlives it
// CHECK-SAME: owned scope-bound local disposed at the end of its block
// CHECK-SAME: (§28.3 / §28.9
