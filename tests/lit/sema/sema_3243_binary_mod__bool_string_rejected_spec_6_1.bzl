// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Regression for #3243 (v0.256 follow-up to narrow #3109). The pre-fix
// binary `mod` arm in `src/sema/ExprTypecheck_Exprs.cpp` rejected only
// `float`/`double` and `char mod char`; `bool` and `string` operands fell
// through to codegen and aborted with `IR verification failed (malformed
// IR emitted)`. The fix rejects `bool`/`string` on either side at sema,
// paralleling the compound `mod=` arm in `ExprTypecheck.cpp` (~11940).

module M;
class C
  public static method Main(): int
    var b: bool := true;
    return int(b mod 2);
  end Main;
end C;

// CHECK: `mod` is not valid on `bool`
// CHECK: `and`/`or`/`not`
// CHECK: §6.1