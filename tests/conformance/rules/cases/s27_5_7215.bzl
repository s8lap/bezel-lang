//! case: s27_5_7215
//! doc: spec
//! section: 27.5
//! title: a closure over constructor locals stored in a field is still
//!   callable after the constructor returns
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 22.7
//! expect: run
//! stdout: 20990
module Conf;

public class H
  public f: () -> int;

  public constructor(n: int)
    var s := "abc" + n.ToString();
    var g := lambda(): int => s.Length + n;
    f := g;
  end constructor;

  public method Run(): int => f();
end H;

public class Program
  public static method Main()
    var tot := 0;
    for var i in 0..<200 do
      using var h := new H(i);
      tot := tot + h.Run();
    end for;
    Console.WriteLine(tot);
  end Main;
end Program;
