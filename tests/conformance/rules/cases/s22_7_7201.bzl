//! case: s22_7_7201
//! doc: spec
//! section: 22.7
//! title: a static constructor storing a closure over its local string
//!   leaves it callable
//! rule: A static constructor runs once before the type's **first use**
//! cites: 22.7, 27.5
//! expect: run
//! stdout: 1600
module Conf;

public class C
  public static readonly F: () -> int;

  static constructor()
    var s := "static-" + "x";
    var f := lambda(): int => s.Length;
    F := f;
  end constructor;
end C;

public class Program
  public static method Main()
    var i := 0;
    var tot := 0;
    while i < 200 do
      tot := tot + C.F();
      i := i + 1;
    end while;
    Console.WriteLine(tot);
  end Main;
end Program;
