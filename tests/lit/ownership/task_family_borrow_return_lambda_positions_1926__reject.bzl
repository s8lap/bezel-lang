// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §28.3 "Task-family calls produce owned frames" (v0.190, SB-01) — issue #1926
// (SB01-B on LAMBDA bodies at the member positions #1919 left unreached).
//
// #1919 enforced the callee-side borrow-return prohibition on a lambda whose
// declared return type is an unmarked `Task`-family type, but only when the
// lambda was assigned to a typed LOCAL (method block / expression bodies). The
// same identity lambda `(t) => t` in a FIELD INITIALIZER, an expression-bodied
// property GETTER, or an expression-bodied OPERATOR compiled CLEAN and would
// HANG at runtime exactly as the #1919 method/local form did. #1926 wires the
// same pre-pass into those positions (supplying the position's declared function
// type as the effective-return fallback, since the type-checker leaves a
// lambda's `funcReturn` un-inferred there).
//
// Fails-without / passes-with: before the fix the compiler emitted nothing for
// these; this CHECK pins the corrected diagnostic and fails against the pre-fix
// compiler.

module M;
import System.Threading;

class C
  // REJECT: a field-initializer identity lambda.
  public echo: (Task<int>) -> Task<int> := lambda(t) => t;

  // REJECT: an expression-bodied getter whose body is the identity lambda.
  public property Echo: (Task<int>) -> Task<int> => lambda(t) => t;

  // REJECT: an expression-bodied operator whose body is the identity lambda.
  public static method operator +(a: C, b: C): (Task<int>) -> Task<int> => lambda(t) => t;

  public static method Main(): int return 0; end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow may not be returned through the non-`async` `Task`-family return of a lambda
// CHECK-SAME: (§28.3 "Task-family calls produce owned frames", v0.190, SB-01)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow may not be returned through the non-`async` `Task`-family return of a lambda
// CHECK-SAME: (§28.3 "Task-family calls produce owned frames", v0.190, SB-01)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow may not be returned through the non-`async` `Task`-family return of a lambda
// CHECK-SAME: (§28.3 "Task-family calls produce owned frames", v0.190, SB-01)
