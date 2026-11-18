//! case: s28_2_6705
//! doc: spec
//! section: 28.2
//! title: a kill in a catch arm that rethrows does not dominate the use after the try
//! rule: on the same variable that dominates the use site on every path is a **compile error**
//! cites: 28.3
//! expect: run
//! stdout: 1
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
    catch
      InvalidOperationException => do d.Dispose(); throw; end,
    end try;
    return d.Ping();
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;
