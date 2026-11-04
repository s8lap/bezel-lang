//! case: s2_7_0809
//! doc: spec
//! section: 2.7
//! title: a case-pattern binding named Math shadows the built-in in its arm and not after the statement
//! rule: If it resolves to a *value* (a local, field, or property), the chain is member access.
//! cites: 2.7, 2.3
//! expect: run
//! output: 3.25
//!   4
//! issue: 8019
module Conf;

public class Sh
  public property Pi: double => 3.25;
end Sh;

public class Program
  public static method Main()
    var o: Object := new Sh();
    case o of
      var Math: Sh => Console.WriteLine(Math.Pi),
      _ => Console.WriteLine("other"),
    end;
    Console.WriteLine(Math.Sqrt(16.0));
  end Main;
end Program;
