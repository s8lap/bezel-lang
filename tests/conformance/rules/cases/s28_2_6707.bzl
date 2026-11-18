//! case: s28_2_6707
//! doc: spec
//! section: 28.2
//! title: a span used only before the disposal of its source is not rejected
//! rule: a disposal after the borrow's last use changes no verdict
//! cites: 28.3, 9.7
//! expect: run
//! stdout: 4
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
    var xs: Array<int> := [1, 2, 3];
    var s := xs.AsSpan(1, 2);
    var r := s[0] + s.Length;
    xs.Dispose();
    return r;
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;
