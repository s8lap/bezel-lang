//! case: s28_2_4802
//! doc: spec
//! section: 28.2
//! title: a holder is invoked after a try whose throw skips the disposal of its object
//! rule: A holder whose source was disposed on only some paths to the use
//! cites: 28.3, 27.6
//! expect: run
//! output: 1800
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
  public static method MayThrow(k: bool): int
    if k then
      throw new InvalidOperationException("x");
    end;
    return 1;
  end MayThrow;

  public static method Run(k: bool): int
    var d := new D(7);
    var f := d.Ping;
    var r := 0;
    try
      r := C.MayThrow(true);
      d.Dispose();
    catch
      var ex: InvalidOperationException => do r := 2; end
    end;
    return r + f();
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
