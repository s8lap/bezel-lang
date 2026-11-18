//! case: s28_2_6711
//! doc: spec
//! section: 28.2
//! title: a case scrutinee member access after the kill is an error
//! rule: A use of a local variable — a read, a borrow taken from it, a member access, an argument position, a consume — after an explicit `Dispose()`
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
  public static method Run(): int
    var d := new Res(1);
    d.Dispose();
    var r := 0;
    case d.Ping() of
      0 => r := 1,
      _ => r := 2
    end case;
    return r;
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;
