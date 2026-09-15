//! case: s14_5_0006
//! doc: spec
//! section: 14.5
//! title: a record's type arguments are never inferred from constructor arguments
//! rule: Class-, record-, and interface-level type arguments are never
//!   inferred from constructor arguments: `new Pair(1, "x")` against
//!   `record Pair<TF, TS>(First: TF, Second: TS)` is a compile error
//!   directing to explicit arguments (§14.5).
//! cites: 14.5
//! expect: reject
//! diagnostic: type argument
module Conf;

public record Pair<TF, TS>(First: TF, Second: TS);

public class Program
  public static method Main()
    var p := new Pair(1, "x");
    Console.WriteLine(p.First);
  end Main;
end Program;
