//! case: l2_3_0015
//! doc: stdlib
//! section: 2.3
//! title: an explicit cast to one parameter type resolves the ambiguous Convert.ToString
//! rule: stdlib §2.3: "the remedy is an explicit cast to one parameter type, or
//!     the value's own `ToString()`". §22.6 agrees: "The caller resolves the
//!     ambiguity by adding an explicit cast (§5.2) to an argument so that exactly
//!     one candidate becomes an exact match". Casting the `byte` value to `int`
//!     makes the `int` form an exact match, so the call renders the value.
//! cites: 2.3, 5.2, 22.6
//! expect: run
//! stdout: 200
module Conf;

public class Program
  public static method Main()
    var b: byte := 200;
    Console.WriteLine(Convert.ToString(int(b), 10));
  end Main;
end Program;
