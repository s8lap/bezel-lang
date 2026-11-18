//! case: s28_2_6704
//! doc: spec
//! section: 28.2
//! title: a second Dispose of the same binding is the runtime no-op and draws a warning
//! rule: a second `Dispose()` or `DisposeAsync()`, or a second discard, of the same binding is the runtime no-op
//! cites: 28.3
//! expect: run
//! warns: yes
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
    d.Dispose();
    d.Dispose();
    return 1;
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;
