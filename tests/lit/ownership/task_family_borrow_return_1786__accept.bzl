// §28.3 "Task-family calls produce owned frames" (v0.190, SB-01) — issue #1786
// (keystone sub-task SB01-B, the callee-return-check side): the ADMIT set the
// callee-return check must NOT reject.
//
// A non-`async` `Task`-family return admits any source an `owned` return
// admits: a forwarded task-returning call, a completed-task factory (§30.10),
// `null` on a nullable path, and the explicit `owned Task` spelling. An
// `async` body is auto-satisfied — its return *is* its own fresh state-machine
// frame, and its return expression is the awaited value (type T), not a task,
// so it must never be classified by this check.
//
// This guards the fix against over-rejecting — especially the forward call
// (`return Produce();`) and the async body, whose false-rejection would break
// idiomatic §30 code.
//
// ACCEPT / NO-DIAGNOSTIC control: `bezelc -c` must exit zero and emit nothing.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
import System.Threading;

class Conn public method Use(): int => 1; end Conn;

class Svc
  private _c: Conn;
  public constructor(c: Conn) _c := c; end constructor;

  // A fresh async task producer used as a forward source below.
  public async method Produce(): Task<Conn> return _c; end Produce;

  // ADMIT: forward a task-returning call (non-`async` body).
  public method Forward(): Task<Conn> return Produce(); end Forward;

  // ADMIT: an `async` body — the return expression is the awaited value (a
  // Conn), not a task; the async state machine is its own fresh frame.
  public async method AsyncBody(): Task<Conn> return _c; end AsyncBody;

  // ADMIT: `null` on a nullable `Task`-family return.
  public method Maybe(): Task<Conn>? return null; end Maybe;

  // ADMIT: completed-task factories (§30.10) forwarded through a plain return.
  public method FromValue(): Task<int> return Task.FromResult<int>(5); end FromValue;
  public method Cancelled(): Task return Task.FromCancelled(); end Cancelled;
  public method Delayed(): Task return Task.Delay(0); end Delayed;

  // ADMIT: the explicit `owned Task` return spelling (returnOwned_ path,
  // unchanged — a fresh factory production transferred to the caller).
  public method Owned(): owned Task return Task.FromCancelled(); end Owned;

  // ADMIT: expression-body forward.
  public method Wait(): Task => Task.Delay(0);
end Svc;

// NODIAG-NOT: error
// NODIAG-NOT: §
