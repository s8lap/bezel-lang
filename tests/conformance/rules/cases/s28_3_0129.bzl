//! case: s28_3_0129
//! doc: spec
//! section: 28.3
//! title: a throwing finally on the return edge leaves the returned value to the callee's unwind
//! rule: if one raises — a `finally` that throws, a `using var`'s or an
//!   auto-dropped local's disposer that throws (§28.4) — the return is
//!   abandoned and no caller-side holder ever exists — the call completes by
//!   the exception unless a handler in the callee catches it — and the slot's
//!   value is disposed by the unwind
//! cites: 28.3, 28.10, 31.2
//! expect: run
//! output: closed 1
//!   caught in callee
//!   got 2
//!   closed 2
//! issue: 5809
module Conf;

public class Res
  public property N: int;

  public constructor(n: int)
    N := n;
  end constructor;

  destructor
    Console.WriteLine($"closed {N}");
  end destructor;
end Res;

public class Demo
  public static method Make(): owned Res
    try
      try
        return new Res(1);
      finally
        throw new InvalidOperationException("boom");
      end try;
    catch
      InvalidOperationException => Console.WriteLine("caught in callee")
    end try;
    return new Res(2);
  end Make;

  public static method Main()
    using var got := Make();
    Console.WriteLine($"got {got.N}");
  end Main;
end Demo;
