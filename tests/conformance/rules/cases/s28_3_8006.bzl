//! case: s28_3_8006
//! doc: spec
//! section: 28.3
//! title: a generic method returning a nested lambda's copy of its T parameter at T = string
//!   yields the text
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 28.3, 14.1
//! expect: run
//! stdout: hi
//! notes: Pins #8987's generic control: reading a captured plain local of type T from a
//!   nested lambda stays admitted at T = string.
module Conf;
public class Program
  public static method Pick<T>(x: T): T
    var outer := lambda(): T => do
      var l: T := x;
      var g := lambda(): T => do return l; end;
      return g();
    end;
    return outer();
  end Pick;
  public static method Main(): int
    Console.WriteLine(Program.Pick<string>("hi"));
    return 0;
  end Main;
end Program;
