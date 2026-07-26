// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Companion REJECT to the #1826 ACCEPT test
// (async_owningtask_discard_1826__discard_accepted_30_5_17_13.bzl).
//
// §30.5: "an `await` whose result is discarded, or appears in a non-
// owned-consuming expression position — the await is a compile error".
//
// The #1826 fix recognises the EXPLICIT `_ := await ...;` discard (§17.13)
// as an owned-consuming position, but a BARE statement-form await
// (`await t;` — the value flows nowhere) is still rejected: the awaiting
// position is neither a binding nor an owned sink nor a discard sink.
// Without #1826 the diagnostic message did not list `_ := ...` as an
// admitted position either way; this test pins that the post-#1826
// message still triggers for the bare-statement form.
//
// NB: the post-#1826 diagnostic added "`an explicit `_ := ...` discard
// (§17.13)`" to its admitted-positions list — the bare-`await t;` line
// below is still rejected because `t`'s await result has no consumer.

module M;
import System.Threading;
class Cell
  public constructor() end constructor;
end Cell;

class C
  public static async method MakeAsync(): OwningTask<Cell>
    return new Cell();
  end MakeAsync;

  public static async method Main(): Task<int>
    using var t := C.MakeAsync();
    await t;          // bare-statement discard; result dropped — §30.5
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: awaiting an `OwningTask<T>` (Pattern B) must appear in an owned-consuming context
// CHECK-SAME: (§30.5
