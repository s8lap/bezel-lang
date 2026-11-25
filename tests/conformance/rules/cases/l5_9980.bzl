//! case: l5_9980
//! doc: stdlib
//! section: 5
//! title: bare static, bare instance, this, other-instance and type-qualified references all feed Select
//! rule: is **retained by the returned query value until enumeration**, not just
//!   for the duration of the call
//! cites: 5, 27.6, 2.7
//! expect: run
//! issue: 9292
//! stdout: 63900
module Conf;

public class Program
  public static method SConv(x: int): int => x + 100;
  public method Conv(x: int): int => x + 1;

  public method Run(o: Program): int
    using var xs: Array<int> := [1, 2, 3];
    var t := 0;
    for var y in xs.Select(SConv) do t := t + y; end;
    for var y in xs.Select(Conv) do t := t + y; end;
    for var y in xs.Select(this.Conv) do t := t + y; end;
    for var y in xs.Select(o.Conv) do t := t + y; end;
    for var y in xs.Select(Program.SConv) do t := t + y; end;
    return t;
  end Run;

  public static method Main()
    var total := 0;
    for var j in 0 ..< 100 do
      using var p := new Program();
      using var q := new Program();
      total := total + p.Run(q);
    end;
    Console.WriteLine(total);
  end Main;
end Program;
