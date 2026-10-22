//! case: s12_7_0040
//! doc: spec
//! section: 12.7
//! title: an is test on a StringBuilder and on a nullable StringBuilder answers for the value's instance
//! rule: "`is T` tests whether a value is an instance of `T` and produces `bool`" ([§12.7]). "`not (x is T)` is a different expression (a boolean negation of a relational test) with the same truth value" ([§17.4]). A null value is an instance of no type, so `n is StringBuilder` is false and `n is not StringBuilder` is true; a present `StringBuilder` is an instance of its own type.
//! cites: 17.4
//! expect: run
//! output: True
//!   False
//!   False
//!   True
//!   True
module Conf;
public class Program
  public static method Main()
    var sb := new StringBuilder();
    Console.WriteLine(sb is StringBuilder);
    Console.WriteLine(sb is not StringBuilder);
    var n: StringBuilder? := null;
    Console.WriteLine(n is StringBuilder);
    Console.WriteLine(n is not StringBuilder);
    var n2: StringBuilder? := new StringBuilder();
    Console.WriteLine(n2 is StringBuilder);
  end Main;
end Program;
