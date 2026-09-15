//! case: s14_5_0007
//! doc: spec
//! section: 14.5
//! title: explicit type arguments construct a generic record
//! rule: The remedy for uninferred record-level type arguments is explicit
//!   arguments: `new Pair<int, string>(1, "x")` (§14.5).
//! cites: 14.5
//! expect: run
//! stdout: 1
module Conf;

public record Pair<TF, TS>(First: TF, Second: TS);

public class Program
  public static method Main()
    var p := new Pair<int, string>(1, "x");
    Console.WriteLine(p.First);
  end Main;
end Program;
