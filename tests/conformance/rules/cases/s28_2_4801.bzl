//! case: s28_2_4801
//! doc: spec
//! section: 28.2
//! title: a holder is invoked when its object is disposed only in a branch that returns
//! rule: A holder whose source was disposed on only some paths to the use
//! cites: 28.3, 27.6
//! expect: run
//! output: 1400
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
  public static method Run(k: bool): int
    var d := new D(7);
    var f := d.Ping;
    if not k then
      d.Dispose();
      return 0;
    end;
    return f();
  end Run;

  public static method Main()
    var total := 0;
    var i := 0;
    while i < 200 do
      total := total + C.Run(i >= 0);
      i := i + 1;
    end;
    Console.WriteLine(total);
  end Main;
end C;
