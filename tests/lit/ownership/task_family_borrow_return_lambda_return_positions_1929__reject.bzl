// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §28.3 "Task-family calls produce owned frames" (v0.190, SB-01) — issue #1929
// (SB01-B on LAMBDA bodies at the RETURN positions #1919/#1926 left unreached).
//
// The SB01-B classifier keys on a lambda's inferred function `funcReturn`, which
// the type-checker leaves null in RETURN position (it is populated only off the
// typed-local #829 hint path). So an identity `Task`-family lambda `(t) => t`
// returned from an expression-bodied METHOD, a block method `return`, or a BLOCK
// getter `return` compiled CLEAN and would HANG at runtime exactly as the #1919
// method/local form did. #1929 threads the enclosing declared return function
// type in as the effective-return fallback for the returned lambda.
//
// Fails-without / passes-with: before the fix the compiler emitted nothing for
// these; this CHECK pins the corrected diagnostic and fails against the pre-fix
// compiler.

module M;
import System.Threading;

class C
  // REJECT: an expression-bodied METHOD whose body IS the identity lambda.
  public static method MakeExpr(): (Task<int>) -> Task<int> => (t) => t;

  // REJECT: a block method body that RETURNS the identity lambda.
  public static method MakeBlock(): (Task<int>) -> Task<int>
    return (t) => t;
  end MakeBlock;

  // REJECT: a BLOCK getter accessor that RETURNS the identity lambda.
  public property Echo: (Task<int>) -> Task<int>
    get => do return (t) => t; end;
  end Echo;

  public static method Main(): int return 0; end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow may not be returned through the non-`async` `Task`-family return of a lambda
// CHECK-SAME: (§28.3 "Task-family calls produce owned frames", v0.190, SB-01)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow may not be returned through the non-`async` `Task`-family return of a lambda
// CHECK-SAME: (§28.3 "Task-family calls produce owned frames", v0.190, SB-01)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow may not be returned through the non-`async` `Task`-family return of a lambda
// CHECK-SAME: (§28.3 "Task-family calls produce owned frames", v0.190, SB-01)
