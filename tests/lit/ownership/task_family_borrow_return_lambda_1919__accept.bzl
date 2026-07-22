// §28.3 "Task-family calls produce owned frames" (v0.190, SB-01) — issue #1919
// (SB01-B on LAMBDA / function-value bodies): the ADMIT set the lambda-body
// return check must NOT reject.
//
// A non-`async` `Task`-family lambda return admits any source an `owned` return
// admits: a forwarded task-returning call, a completed-task factory (§30.10),
// and an `owned` parameter. An `async` lambda is auto-satisfied — its body is
// the awaited value, not a task. A lambda returning a borrow of a NON-`Task`
// type is not this rule's case at all. This guards the fix against
// over-rejecting idiomatic §30 lambdas.
//
// ACCEPT / NO-DIAGNOSTIC control: `bezelc -c` must exit zero and emit nothing.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
import System.Threading;

class C
  public static async method Producer(): Task<int> return 5; end Producer;

  public static method Main(): int
    // ADMIT: forward a fresh task-returning call (the dominant shape).
    var fresh: () -> Task<int> := lambda() => Producer();
    // ADMIT: an `async` lambda — body is the awaited value, not a task.
    var asyncl: () -> Task<int> := async lambda() => await Producer();
    // ADMIT: a completed-task factory (§30.10).
    var factory: () -> Task<int> := lambda() => Task.FromResult<int>(5);
    // ADMIT: an `owned` parameter returned through a plain-`Task` return.
    var ownedp: (owned Task<int>) -> Task<int> := lambda(owned t) => t;
    // UNAFFECTED: a NON-`Task` (value-typed) identity lambda.
    var id: (int) -> int := lambda(x) => x;
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
