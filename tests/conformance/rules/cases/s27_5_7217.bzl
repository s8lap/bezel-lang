//! case: s27_5_7217
//! doc: spec
//! section: 27.5
//! title: a closure over `this` stored from a constructor still runs after
//!   the constructor returns
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 22.7
//! expect: run
//! stdout: 39800
module Conf;

public class H
  public X: int;
  public f: () -> int;

  public constructor(n: int)
    X := n;
    var g := lambda(): int => this.X * 2;
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
