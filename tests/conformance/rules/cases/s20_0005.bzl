//! case: s20_0005
//! doc: spec
//! section: 20
//! title: a selected throw branch propagates its exception out of the if expression
//! rule: "a `throw` branch is admitted in any branch position" and "the first
//!   `true` condition selects its branch, the `else` branch is selected when none
//!   holds" — here the condition is false, so the `throw` branch is selected and
//!   its exception propagates.
//! cites: 20, 17.12
//! expect: run
//! stdout: thrown
module Conf;

public class Program
  public static method Main()
    var c := false;
    try
      var v := (if c then 1 else throw new InvalidOperationException("nope"));
      Console.WriteLine(v);
    catch
      var ex: InvalidOperationException => Console.WriteLine("thrown")
    end try;
  end Main;
end Program;
