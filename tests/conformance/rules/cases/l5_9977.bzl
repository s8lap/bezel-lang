//! case: l5_9977
//! doc: stdlib
//! section: 5
//! title: a bare instance method name passed to OrderBy as the key selector orders the enumeration
//! rule: is **retained by the returned query value until enumeration**, not just
//!   for the duration of the call
//! cites: 5, 27.6, 2.7
//! expect: run
//! issue: 9292
//! stdout: 4321
module Conf;

public class Program
  public method Key(x: int): int => 0 - x;

  public method Run(): string
    using var xs: Array<int> := [1, 3, 2, 4];
    var s := "";
    for var y in xs.OrderBy(Key) do
      s := s + y.ToString();
    end;
    return s;
  end Run;

  public static method Main()
    var last := "";
    for var j in 0 ..< 100 do
      using var p := new Program();
      last := p.Run();
    end;
    Console.WriteLine(last);
  end Main;
end Program;
