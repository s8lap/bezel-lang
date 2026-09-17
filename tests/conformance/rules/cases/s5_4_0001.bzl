//! case: s5_4_0001
//! doc: spec
//! section: 5.4
//! title: an in-range int converts to its enum constant
//! rule: An integer-to-enum cast E(n) is checked against the declared
//!   constants: it succeeds exactly when n equals the underlying value of
//!   some declared constant of E, and yields that value.
//! cites: 5.4
//! expect: run
//! stdout: Medium
module Conf;

public enum Priority
  Low, Medium, High;
end Priority;

public class Program
  public static method Main()
    var p := Priority(1);
    Console.WriteLine(Priority.NameOf(p));
  end Main;
end Program;
