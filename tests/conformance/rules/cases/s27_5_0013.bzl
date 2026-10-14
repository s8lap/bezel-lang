//! case: s27_5_0013
//! doc: spec
//! section: 27.5
//! title: a write after a two-level nested lambda was formed is seen when that lambda is called
//! rule: Modifications to a captured variable inside the lambda are visible
//!   outside, and vice versa. (v0.338 §27.5)
//! cites: 27.5
//! expect: run
//! output: 5
module Conf;

public class Program
  public static method Main()
    var probe := lambda() => do
      var v := 1;
      var mid := lambda() => lambda() => v;
      var read := mid();
      v := 5;
      return read();
    end;
    Console.WriteLine(probe());
  end Main;
end Program;
