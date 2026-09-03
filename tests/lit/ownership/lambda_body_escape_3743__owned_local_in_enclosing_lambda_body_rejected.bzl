// §28.3 "Local variables, ownership, and borrows" / §28.9 "Runtime-managed
// types" (v0.286), issue #3743: the owned-local ASSIGNMENT-channel escape one
// frame further out than #3727 — the source is declared in an ENCLOSING
// LAMBDA's body, not in the method body.
//
// §28.9: "Capturing an *owned* class-typed local does **not** transfer
// ownership into the closure: the capture is a borrow whose source is the
// local's enclosing scope, borrow-state throughout ... and may not outlive
// that scope (§28.3)".
// §28.3 *Borrow lifetime rules*: "A borrow may not be assigned to a local
// whose scope strictly encloses the borrow's source's scope".
//
// fix #3727 sealed ONE enclosing frame — the first `collectLocalDepths` walked
// — and that walk never enters a `LambdaExpr`, so `r` below was a key of no
// map at all: `checkBorrowStoreEscape` resolved no source and returned before
// any comparison. Measured on the parent (9feef15): rc=0, NO diagnostic, and
// SIGSEGV at run (ASan: heap-use-after-free reading `r.v`). The byte-identical
// program with `r` and `f` hoisted one frame IN has been rejected since #3727.
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
    var outer := lambda(): int => do
      using var r := new R();
      var mk := lambda(): int => do
        f := lambda(): int => r.V();
        return 0;
      end;
      return mk();
    end;
    var t := outer();
    return t + f() + f();
  end Run;
  public static method Main(): int
    Console.WriteLine(Run().ToString());
    return 0;
  end Main;
end C;

// The METHOD-level diagnostic, unchanged — the same check reached through a
// frame it could not reach before, not a second spelling of the rule.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: closure capturing `r` is assigned to `f`, which outlives it
// CHECK-SAME: is an owned scope-bound local disposed at the end of its block
// CHECK-SAME: (§28.3 / §28.9
