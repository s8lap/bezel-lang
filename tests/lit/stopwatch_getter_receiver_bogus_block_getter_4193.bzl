// fix issue 4193 (stdlib §8.1/§23.2-§23.3, v0.298): REJECT control,
// BLOCK-FORM getter (`property P: T get => do ... end; end;`, §23.3) — a
// DIFFERENT lowering arm (`blockGetterOf`/`ensurePropertyBlockGetter`) from
// the expression-bodied sibling
// (stopwatch_getter_receiver_bogus_expr_getter_4193.bzl), so this is not the
// same code path exercised twice. See the accept sibling
// (stopwatch_getter_receiver_accept_4193.bzl) for the full derivation.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;

import System.Diagnostics;

class C
  // With the #5351 fix, Stopwatch.StartNew() returns owned, which cannot be
  // returned from a getter (§28.6). Store in an owned field and return a borrow.
  private owned swBlock: Stopwatch;
  public constructor() swBlock := Stopwatch.StartNew(); end constructor;

  public property SwBlock: Stopwatch get => swBlock; end;

  public method Test(): int
    SwBlock.TotallyBogusBlockGetterMember4193Lit();
    return 0;
  end Test;

  public static method Main(): int
    var c := new C();
    return c.Test();
  end Main;
end C;

// CHECK: `TotallyBogusBlockGetterMember4193Lit` on built-in type `Stopwatch` produced no value, and no dispatch arm emitted any code for this call
