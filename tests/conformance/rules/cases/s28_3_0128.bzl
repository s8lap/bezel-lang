//! case: s28_3_0128
//! doc: spec
//! section: 28.3
//! title: the return slot's value is disposed at the raise, before a callee handler's arm runs
//! rule: the slot's value is disposed by the unwind as an owned local
//!   established at that point would be: **at the raise**, before every
//!   cleanup still pending on the edge that was established before the
//!   `return` statement and before any handler's arm in the callee, whichever
//!   scope the raising cleanup belongs to ... so a handler that catches the
//!   exception finds no returned value left to dispose.
//! cites: 28.3, 28.10, 31.2
//! expect: run
//! output: closed 1
//!   caught in callee
//!   got 3
//!   closed 3
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
  public static method Make(early: bool): owned Res
    try
      using var b := new Boom();
      if early then
        return new Res(1);
      end;
    catch
      InvalidOperationException => Console.WriteLine("caught in callee")
    end try;
    return new Res(3);
  end Make;

  public static method Main()
    using var got := Make(true);
    Console.WriteLine($"got {got.N}");
  end Main;
end Demo;
