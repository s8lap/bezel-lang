//! case: l2_3_0020
//! doc: stdlib
//! section: 2.3
//! title: a narrowed nullable toBase renders the radix form
//! rule: stdlib §2.3: "`ToString(value, toBase)` returns the text
//!     `value.ToString(toBase)` returns (§9.1)". §22.6: "The argument's static
//!     type is its type at the call: a read of a binding §7.4 has narrowed
//!     non-null is of its non-nullable type". The narrowed `r` is an `int` at
//!     the call, so the `int` form is applicable and the radix form is rendered.
//! cites: 2.3, 7.4, 9.1, 22.6
//! expect: run
//! stdout: ff
module Conf;

public class Program
  public static method Main()
    var r: int? := 16;
    if r <> null then
      Console.WriteLine(Convert.ToString(255, r));
    end;
  end Main;
end Program;
