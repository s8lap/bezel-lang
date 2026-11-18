//! case: s28_2_6710
//! doc: spec
//! section: 28.2
//! title: a type test of the variable after the kill is a read and an error
//! rule: A use of a local variable — a read, a borrow taken from it, a member access, an argument position, a consume — after an explicit `Dispose()`
//! cites: 28.3, 12.7
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
    if d is Res then return 1; end if;
    return 0;
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;
