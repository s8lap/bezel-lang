//! case: l5_9975
//! doc: stdlib
//! section: 5
//! title: a bare instance method name passed to Select is retained by the query and applied at enumeration
//! rule: is **retained by the returned query value until enumeration**, not just
//!   for the duration of the call
//! cites: 5, 27.6, 2.7
//! expect: run
//! issue: 9292
//! stdout: 900
module Conf;

public class Program
  public method Conv(x: int): int => x + 1;

  public method Run(): int
    using var xs: Array<int> := [1, 2, 3];
    var t := 0;
    for var y in xs.Select(Conv) do
      t := t + y;
    end;
    return t;
  end Run;

  public static method Main()
    var total := 0;
    for var j in 0 ..< 100 do
      using var p := new Program();
      total := total + p.Run();
    end;
    Console.WriteLine(total);
  end Main;
end Program;
