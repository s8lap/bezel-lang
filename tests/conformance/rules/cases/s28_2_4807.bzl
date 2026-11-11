//! case: s28_2_4807
//! doc: spec
//! section: 28.2
//! title: a holder whose last use is in a loop before the object's disposal is not rejected
//! rule: a disposal after the borrow's last use changes no verdict
//! cites: 28.3, 27.6
//! expect: run
//! output: 4200
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
    var acc := 0;
    var j := 0;
    while j < 3 do
      acc := acc + f();
      j := j + 1;
    end;
    d.Dispose();
    return acc;
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
