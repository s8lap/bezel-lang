// fix issue 4193 (stdlib §8.1/§23.2-§23.3, v0.298): REJECT control, EXPRESSION
// -bodied getter form (`property P: T => expr;`). See the accept sibling
// (stopwatch_getter_receiver_accept_4193.bzl) for the full derivation.
//
// Before this fix, `SwExpr.TotallyBogusMember();` compiled with ZERO
// diagnostics: evaluating the bare `SwExpr` identifier runs its getter
// (`Stopwatch.StartNew()`, a real side-effecting call) before the member name
// is ever compared, so `emittedSideEffectSince(dispatchEntry)` read true for
// the wrong reason and masked the `ftRecv`/#2935 Error
// (`LlvmBackend_Calls2.cpp`) that fires correctly for the same bogus member
// through a LOCAL-bound receiver (`var s := SwExpr; s.TotallyBogusMember();`
// — tests/codegen/builtin_receiver_unlowered_member_2935_test.cpp's own
// `bogus_member_on_stopwatch_rejected_stdlib_8_1`).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
// RUN: %not %bezelc --checked-disposal -c %s 2>&1 | FileCheck %s
// RUN: %not %bezelc --unchecked-disposal -c %s 2>&1 | FileCheck %s
// #4602 (§28.2, v0.338): both disposal modes pin the rejection. A checked
// build appends an escape mark to the getter result, which once read as
// "an arm handled it" and dropped the diagnostic.

module M;

import System.Diagnostics;

class C
  // With the #5351 fix, Stopwatch.StartNew() returns owned, which cannot be
  // returned from a getter (§28.6). Store in an owned field and return a borrow.
  private owned swExpr: Stopwatch;
  public constructor() swExpr := Stopwatch.StartNew(); end constructor;

  public property SwExpr: Stopwatch get => swExpr; end;

  public method Test(): int
    SwExpr.TotallyBogusExprGetterMember4193Lit();
    return 0;
  end Test;

  public static method Main(): int
    var c := new C();
    return c.Test();
  end Main;
end C;

// CHECK: `TotallyBogusExprGetterMember4193Lit` on built-in type `Stopwatch` produced no value, and no dispatch arm emitted any code for this call
