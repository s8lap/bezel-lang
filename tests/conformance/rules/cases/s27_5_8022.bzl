//! case: s27_5_8022
//! doc: spec
//! section: 27.5
//! title: a generic method copying a lambda result into a second T? local runs at T = string
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 14.1, 28.9
//! expect: run
//! stdout: 600
//! notes: Pins the #9007 fix-forward stack (consumers of a `T`-returning lambda call
//!   classify through one nullability-aware predicate); the corpus runner has no leak
//!   leg, so this asserts the program output.
module Conf;
public class Program
  public static method Use<T>(p: T?): int
    var f := lambda() => p;
    var r := f();
    var q: T? := r;
    return $"{q}".Length;
  end Use;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var s := "hi" + "x";
      t := t + Program.Use<string>(s);
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
