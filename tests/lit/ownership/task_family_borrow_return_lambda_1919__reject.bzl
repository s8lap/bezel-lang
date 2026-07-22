// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §28.3 "Task-family calls produce owned frames" (v0.190, SB-01) — issue #1919
// (SB01-B on LAMBDA / function-value bodies).
//
// The callee-side rule applies to "a non-`async` method **or lambda**": every
// return path of a lambda whose declared return type is an unmarked
// `Task`-family type must supply an OWNED task, so returning a BORROW — a plain
// parameter, or a captured `Task`-family field read — is a compile error. This
// was enforced on method bodies but NOT on lambda bodies (which are not
// otherwise ownership-analysed), so the identity lambda below COMPILED CLEAN and
// then HUNG at runtime (double-free / async-state corruption) while the
// equivalent method `Echo(t: Task<int>): Task<int> return t;` was rejected.
//
// Fails-without / passes-with: before the fix the compiler emitted nothing for
// these; this CHECK pins the corrected diagnostic and fails against the pre-fix
// compiler.

module M;
import System.Threading;

class Cache
  private _cachedTask: Task<int>;
  public constructor(t: Task<int>) _cachedTask := t; end constructor;

  // REJECT: a lambda capturing and returning a `Task`-family field read (a
  // borrow of its storage) through a plain-`Task` return.
  public method Make(): () -> Task<int>
    var f: () -> Task<int> := lambda() => _cachedTask;
    return f;
  end Make;
end Cache;

class C
  public static method Main(): int
    // REJECT: expression-bodied identity lambda `(t) => t` — the issue's repro.
    var echo: (Task<int>) -> Task<int> := lambda(t) => t;
    // REJECT: the block-body sibling.
    var echo2: (Task<int>) -> Task<int> := lambda(t) => do return t; end;
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow may not be returned through the non-`async` `Task`-family return of a lambda
// CHECK-SAME: (§28.3 "Task-family calls produce owned frames", v0.190, SB-01)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow may not be returned through the non-`async` `Task`-family return of a lambda
// CHECK-SAME: (§28.3 "Task-family calls produce owned frames", v0.190, SB-01)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow may not be returned through the non-`async` `Task`-family return of a lambda
// CHECK-SAME: (§28.3 "Task-family calls produce owned frames", v0.190, SB-01)
