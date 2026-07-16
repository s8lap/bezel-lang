// §28.3 "Task-family calls produce owned frames" (v0.190, SB-01) — issue #1929
// (SB01-B on LAMBDA bodies at the RETURN positions): the ADMIT set the returned-
// lambda check must NOT reject.
//
// A non-`async` `Task`-family lambda return admits any source an `owned` return
// admits: a forwarded task-returning call, a completed-task factory (§30.10),
// and an `owned` parameter through an `owned` return. An `async` lambda is
// auto-satisfied. A lambda returning a borrow of a NON-`Task` type is not this
// rule's case at all. A non-return-direct local lambda must NOT inherit the
// method's declared return type. This guards the #1929 fix (its return-position
// declared-type fallback) against over-rejecting idiomatic §30 lambdas.
//
// ACCEPT / NO-DIAGNOSTIC control: `bezelc -c` must exit zero and emit nothing.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
import System.Threading;

class C
  public static async method Producer(): Task<int> return 5; end Producer;

  // ADMIT (expr method): forward a fresh task-returning call.
  public static method MkFwdExpr(): () -> Task<int> => () => Producer();

  // ADMIT (block method): forward a fresh task-returning call from a `return`.
  public static method MkFwdBlock(): () -> Task<int>
    return () => Producer();
  end MkFwdBlock;

  // ADMIT (block getter): forward a fresh task-returning call from a `return`.
  public property Echo: () -> Task<int>
    get => do return () => Producer(); end;
  end Echo;

  // ADMIT (block method): an `async` returned lambda.
  public static method MkAsync(): () -> Task<int>
    return async () => await Producer();
  end MkAsync;

  // ADMIT (block method): a completed-task factory (§30.10).
  public static method MkFactory(): () -> Task<int>
    return () => Task.FromResult<int>(5);
  end MkFactory;

  // ADMIT (expr method): an `owned` parameter returned through an `owned` return.
  public static method MkOwned(): (owned Task<int>) -> owned Task<int>
    => (owned t) => t;

  // UNAFFECTED (block method): a NON-`Task` (value-typed) identity lambda.
  public static method MkId(): (int) -> int
    return (x) => x;
  end MkId;

  // SURGICAL (block method): a non-return-direct local lambda in a
  // `Task`-returning method must NOT inherit the declared return type.
  public static method MkSurgical(): () -> Task<int>
    var g := (x: int) => x;
    var h: int := g(3);
    return () => Task.FromResult<int>(5);
  end MkSurgical;

  public static method Main(): int return 0; end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
