//! case: l5_9979
//! doc: stdlib
//! section: 5
//! title: a bare instance method name on a record receiver is captured by copy and applied at enumeration
//! rule: is **retained by the returned query value until enumeration**, not just
//!   for the duration of the call
//! cites: 5, 27.6, 2.7
//! expect: run
//! issue: 9292
//! stdout: 2100
module Conf;

public record R(A: int)
  public method Conv(x: int): int => x + A;

  public method Run(): int
    using var xs: Array<int> := [1, 2, 3];
    var t := 0;
    for var y in xs.Select(Conv) do
      t := t + y;
    end;
    return t;
  end Run;
end R;

public class Program
  public static method Main()
    var total := 0;
    for var j in 0 ..< 100 do
      var r := new R(5);
      total := total + r.Run();
    end;
    Console.WriteLine(total);
  end Main;
end Program;
