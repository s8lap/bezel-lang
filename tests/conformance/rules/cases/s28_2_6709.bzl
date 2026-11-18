//! case: s28_2_6709
//! doc: spec
//! section: 28.2
//! title: a while condition reading the variable after the kill is an error
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
    var s := 0;
    while d.Ping() > s do
      s := s + 1;
    end while;
    return s;
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;
