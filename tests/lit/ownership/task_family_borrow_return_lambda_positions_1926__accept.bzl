// §28.3 "Task-family calls produce owned frames" (v0.190, SB-01) — issue #1926
// (SB01-B on LAMBDA bodies at the field-initializer / getter / operator
// positions): the ADMIT set the newly-wired positions must NOT reject.
//
// A non-`async` `Task`-family lambda return admits any source an `owned` return
// admits: a forwarded task-returning call, a completed-task factory (§30.10),
// and an `owned` parameter. An `async` lambda is auto-satisfied. A lambda
// returning a borrow of a NON-`Task` type is not this rule's case. This guards
// the #1926 wiring (and its declared-type fallback) against over-rejecting
// idiomatic lambdas in the new positions.
//
// ACCEPT / NO-DIAGNOSTIC control: `bezelc -c` must exit zero and emit nothing.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
import System.Threading;

class C
  public static async method Producer(): Task<int> return 5; end Producer;

  // ADMIT (field): forward a fresh task-returning call.
  public fwd: () -> Task<int> := () => Producer();
  // ADMIT (field): an `async` lambda — body is the awaited value, not a task.
  public asyncl: () -> Task<int> := async () => await Producer();
  // ADMIT (field): a completed-task factory (§30.10).
  public factory: () -> Task<int> := () => Task.FromResult<int>(5);
  // UNAFFECTED (field): a NON-`Task` (value-typed) identity lambda.
  public id: (int) -> int := (x) => x;

  // ADMIT (getter): forward a fresh task-returning call from a getter body.
  public property Echo: () -> Task<int> => () => Producer();

  // ADMIT (operator): an `owned` parameter returned through a plain-`Task`
  // return.
  public static method operator +(a: C, b: C): (owned Task<int>) -> Task<int> => (owned t) => t;

  public static method Main(): int return 0; end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
