// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §30.5 / Appendix C.6 (second-await split, v0.193) — issue #1785.
//
// `Mutex.AcquireAsync()` returns a built-in `OwningTask<Lock>` (stdlib §6.2) —
// Pattern B, single-consumer. Bound to a PLAIN owned local (`var t`, movable),
// the first `await t` CONSUMES it; a straight-line SECOND `await t` is a
// compile-time moved-from read.
//
// Regression: `Lock` is a compiler-known class synthesized with a NULL
// declaration, which the ownership pass's await-kind classifier
// (`taskResultArgIsClass`) failed to recognize as a class — so `OwningTask<Lock>`
// was classified NotTask, the Pattern B operand consume never fired, and the
// second await was mis-diagnosed as `t` being "dropped here without disposal"
// instead of the correct single-consumer moved-from error. This CHECK pins the
// corrected diagnostic; the pre-fix compiler emitted the "dropped without
// disposal" message and this test fails against it.

module M;
import System.Threading;

class C
  public async method Acquire(m: Mutex): Task
    var t := m.AcquireAsync();       // plain owned local OwningTask<Lock>
    using var g1 := await t;         // first await consumes t
    using var g2 := await t;         // second await — moved-from read
  end Acquire;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `OwningTask<T>` `t` was already awaited (consumed)
// CHECK-SAME: single-consumer; awaiting it again moves out an already-moved result (§30.5 / §30.4)
