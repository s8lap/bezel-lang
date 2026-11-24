//! case: s27_5_8024
//! doc: spec
//! section: 27.5
//! title: a generic method discarding a lambda call over its T? parameter at null runs
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 14.1, 28.9
//! expect: run
//! stdout: 200
//! notes: Pins the #9007 fix-forward null-argument control for the discarded-result
//!   consumer.
module Conf;
public class Program
  public static method Use<T>(p: T?): int
    var f := lambda() => p;
    f();
    return 1;
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
