//! case: s28_2_6713
//! doc: spec
//! section: 28.2
//! title: a kill by Dispose on one arm and by discard on the other dominates the later use
//! rule: A use of a local variable — a read, a borrow taken from it, a member access, an argument position, a consume — after an explicit `Dispose()`
//! cites: 28.3, 17.13
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
  public static method Pick(): bool => "x".Length > 1;
  public static method Run(): int
    var d := new Res(1);
    if Program.Pick() then
      d.Dispose();
    else
      _ := d;
    end if;
    return d.Ping();
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;
