// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §28.3 "Task-family calls produce owned frames" (v0.190, SB-01) — issue #1786
// (keystone sub-task SB01-B, the callee-return-check side).
//
// A NON-`async` method (or lambda) whose declared return type is an unmarked
// `Task`-family type (`Task`, `Task<T>`) is ownership-transferring for the task
// container — its declared type is the announcement, exactly like an `owned`
// return — so every return path must supply an OWNED task. Returning a BORROW
// is a compile error: a stored task is re-exposed through a property, field, or
// collection slot (whose reads are borrows of their storage), never through a
// non-`owned` method return.
//
// The three borrow forms the spec names, each rejected here:
//   - a bare (implicit-`this`) task-field read      (GetTask)
//   - an explicit `this.`-qualified task-field read (GetTask2)
//   - a plain (non-`owned`) parameter               (Echo)
//
// Fails-without / passes-with: before SB01-B the reject was gated on
// `isOwningTypeHead` (which carries `OwningTask` but NOT the plain `Task` head),
// so all three of these compiled clean — the callee-side laundering hole. This
// CHECK pins the corrected diagnostic; the pre-fix compiler emits nothing and
// this test fails against it.

module M;
import System.Threading;

class Conn public method Use(): int => 1; end Conn;

class Cache
  private _cachedTask: Task<Conn>;
  public constructor(t: Task<Conn>) _cachedTask := t; end constructor;

  // REJECT: bare (implicit-`this`) task-field read.
  public method GetTask(): Task<Conn> return _cachedTask; end GetTask;

  // REJECT: explicit `this.`-qualified task-field read.
  public method GetTask2(): Task<Conn> return this._cachedTask; end GetTask2;

  // REJECT: plain (non-`owned`) parameter returned through a plain `Task` return.
  public method Echo(t: Task<Conn>): Task<Conn> return t; end Echo;
end Cache;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow may not be returned through the non-`async` `Task`-family return
// CHECK-SAME: (§28.3 "Task-family calls produce owned frames", v0.190, SB-01)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow may not be returned through the non-`async` `Task`-family return
// CHECK-SAME: (§28.3 "Task-family calls produce owned frames", v0.190, SB-01)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow may not be returned through the non-`async` `Task`-family return
// CHECK-SAME: (§28.3 "Task-family calls produce owned frames", v0.190, SB-01)
