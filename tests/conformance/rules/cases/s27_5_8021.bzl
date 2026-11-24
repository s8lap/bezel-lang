//! case: s27_5_8021
//! doc: spec
//! section: 27.5
//! title: a generic method calling a lambda over its nullable T? parameter at a null
//!   argument renders null
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 14.1, 28.9
//! expect: run
//! stdout: 800
//! notes: Pins the #9007 fix-forward null-argument control: no release of a null result (the
//!   intermediate fix aborted on the non-null side and the corpus carries the null
//!   side).
module Conf;
public class Program
  public static method Use<T>(p: T?): int
    var f := lambda() => p;
    var r: T? := f();
    if r <> null then return 0; end;
    return 4;
  end Use;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var s := "hi" + "x";
      t := t + Program.Use<string>(null);
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
