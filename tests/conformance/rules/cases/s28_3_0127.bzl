//! case: s28_3_0127
//! doc: spec
//! section: 28.3
//! title: a returned owned value is disposed by the callee's unwind when a return-edge disposer raises
//! rule: The slot's value transfers to the caller only when every cleanup on
//!   the return edge has completed normally; if one raises — a `finally` that
//!   throws, a `using var`'s or an auto-dropped local's disposer that throws
//!   (§28.4) — the return is abandoned and no caller-side holder ever exists
//!   ... and the slot's value is disposed by the unwind
//! cites: 28.3, 28.10, 28.4
//! expect: run
//! output: closed 1
//!   caught
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

public class Boom
  public constructor()
  end constructor;

  destructor
    throw new InvalidOperationException("boom");
  end destructor;
end Boom;

public class Demo
  public static method Make(): owned Res
    var r := new Res(1);
    using var b := new Boom();
    return r;
  end Make;

  public static method Main()
    try
      using var got := Make();
      Console.WriteLine($"got {got.N}");
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end try;
  end Main;
end Demo;
