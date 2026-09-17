//! case: l2_19_0002
//! doc: stdlib
//! section: 2.19
//! title: char conforms to ISteppable<char> through T.Step
//! rule: char conforms to ISteppable<char>, witnessed by the compiler-
//!   provided conformance; ISteppable<T>.Step(value, by) advances in T's
//!   space.
//! cites: 2.19
//! expect: run
//! stdout: b
module Conf;

public class Program
  public static method Advance<T>(v: T): T where T: ISteppable<T> => T.Step(v, 1);

  public static method Main()
    var c := Advance('a');
    Console.WriteLine(c);
  end Main;
end Program;
