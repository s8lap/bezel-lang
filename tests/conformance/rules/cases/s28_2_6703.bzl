//! case: s28_2_6703
//! doc: spec
//! section: 28.2
//! title: a disposal after the alias's last use changes no verdict
//! rule: a disposal after the borrow's last use changes no verdict
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
  public static method Run(): int
    var d := new Res(1);
    var b := d;
    var r := b.Ping();
    d.Dispose();
    return r;
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;
