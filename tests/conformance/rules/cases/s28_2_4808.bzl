//! case: s28_2_4808
//! doc: spec
//! section: 28.2
//! title: a holder invoked after a disposal that only one branch performs is not rejected
//! rule: A holder whose source was disposed on only some paths to the use
//! cites: 28.3, 27.6
//! expect: run
//! output: 7
module Conf;

public class D
  private _n: int;

  public constructor(n: int)
    _n := n;
  end constructor;

  public method Ping(): int => _n;
  public static method Stat(): int => 5;
end D;

public class C
  public static method Run(kill: bool): int
    var d := new D(7);
    var f := d.Ping;
    if kill then
      d.Dispose();
    end;
    return f();
  end Run;

  public static method Main()
    Console.WriteLine(C.Run(false));
  end Main;
end C;
