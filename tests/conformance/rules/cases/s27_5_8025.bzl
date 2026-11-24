//! case: s27_5_8025
//! doc: spec
//! section: 27.5
//! title: a generic method passing a lambda call's T result on to a generic sink runs at T =
//!   string
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 14.1, 28.9
//! expect: run
//! stdout: 1200
//! notes: Pins the #9007 fix-forward stack (consumers of a `T`-returning lambda call
//!   classify through one nullability-aware predicate); the corpus runner has no leak
//!   leg, so this asserts the program output.
module Conf;
public class Program
  public static method Sink<U>(u: U): int
    return $"{u}".Length;
  end Sink;
  public static method Use<T>(p: T): int
    var f := lambda() => p;
    var r := f();
    var q := r;
    return Program.Sink<T>(q) + Program.Sink<T>(r);
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
