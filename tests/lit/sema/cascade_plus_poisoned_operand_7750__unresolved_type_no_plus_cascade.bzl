// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #7750 (§4.6, v0.355; §3.5 name resolution): after an UNRESOLVED TYPE name,
// the local declared with it is untyped. A `string + <that local's member>`
// then saw "exactly one operand is `string`" and printed a CASCADE `+` error
// (once with no location, `:0:0:`). The first error is the real one; an
// operand with no type is poisoned, so §4.6's string-operand rule has nothing
// to say about it. The program is REJECTED (the first error), so the
// diagnostics are filled and the absence asserts below are not vacuous.

module M;
import System;
class C
  public static method Main(): int
    var l := new Nope<int>();
    Console.WriteLine("n=" + l.Count.ToString() + "|" + l.Count.ToString());
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unresolved type name `Nope`
// CHECK-NOT: :0:0:
// CHECK-NOT: requires both operands to be `string`
