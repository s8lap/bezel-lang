//! case: s28_2_6706
//! doc: spec
//! section: 28.2
//! title: a kill in an early-return arm does not dominate the later use
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
  public static method Pick(): bool => "x".Length > 1;
  public static method Run(): int
    var d := new Res(1);
    if Program.Pick() then
      d.Dispose();
      return 0;
    end if;
    return d.Ping();
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;
