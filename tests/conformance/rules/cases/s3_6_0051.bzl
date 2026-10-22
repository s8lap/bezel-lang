//! case: s3_6_0051
//! doc: spec
//! section: 3.6
//! title: a constrained type parameter reads its constraint's CauseChain, one entry per cause
//! rule: "A generic body type-checks under the weakest assumption its constraints establish" ([§3.6]). "copies its type name, message, stack-trace text, and its own `CauseChain` into the new exception's `CauseChain` (outermost cause first, flattened)" ([§31.4]), and `CauseChain` is "empty when no cause" ([§31.4]). So `x.CauseChain` on `T: Exception` yields the one cause of `outer` and nothing for `plain`.
//! cites: 31.4
//! expect: run
//! output: inner
//!   1
//!   0
module Conf;
public class Program
  public static method M<T>(x: T): int where T: Exception
    var n := 0;
    for var c in x.CauseChain do
      Console.WriteLine(c.Message);
      n := n + 1;
    end for;
    return n;
  end M;
  public static method Main()
    using var inner := new Exception("inner");
    using var io := new Exception("outer", inner);
    Console.WriteLine(M(io));
    using var plain := new Exception("p");
    Console.WriteLine(M(plain));
  end Main;
end Program;
