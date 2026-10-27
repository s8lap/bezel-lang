//! case: s22_5_0016
//! doc: spec
//! section: 22.5
//! title: a call to a string member that declares a required parameter and supplies no argument is rejected
//! rule: **Every parameter that declares no default must be bound**, positionally
//!   or by name; leaving one unbound is the opposite disposition from binding one
//!   twice — it makes the candidate **inapplicable**, so at an overloaded name an
//!   applicable sibling still resolves and the error is reported only where no
//!   candidate survives. stdlib §9.6 declares `Equals(other: string): bool` and
//!   `Equals(other: string?, ignoreCase: bool): bool` — neither declares a
//!   default — so a call supplying no argument binds no parameter of either
//!   candidate, neither candidate survives, and the call is a compile error.
//! cites: 22.5, 9.6, 22.6
//! expect: reject
//! diagnostic: argument
module Conf;

public class Program
  public static method Main()
    var b := "ab".Equals();
    Console.WriteLine(b);
  end Main;
end Program;
