//! case: s28_2_6712
//! doc: spec
//! section: 28.2
//! title: a kill in a finally block dominates the use after the try statement
//! rule: on the same variable that dominates the use site on every path is a **compile error**
//! cites: 28.3
//! expect: reject
//! diagnostic: dominat
module Conf;

public class Res
  private _n: int;
  public constructor(n: int)
    _n := n;
  end constructor;
  public method Ping(): int => _n;
end Res;

public class Program
  public static method Boom(kill: bool)
    if kill then throw new InvalidOperationException("x"); end if;
  end Boom;
  public static method Run(): int
    var d := new Res(1);
    try
      Program.Boom(false);
    finally
      d.Dispose();
    end try;
    return d.Ping();
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;
