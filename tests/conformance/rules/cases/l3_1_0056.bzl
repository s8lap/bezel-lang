//! case: l3_1_0056
//! doc: stdlib
//! section: 3.1
//! title: a user-declared StringBuilder's own member is checked against its own declaration
//! rule: §22.5: every parameter that declares no default must be bound, positionally
//!   or by name; leaving one unbound makes the candidate inapplicable, so §22.6
//!   rejects the call. A user-declared type's member surface is its own
//!   declaration, not the built-in surface of stdlib §3.1 - and the arity rule
//!   applies to it unchanged.
//! cites: 22.5, 22.6, 3.1
//! expect: reject
//! diagnostic: not supplied
//! issue: 7207
module Conf;

public class StringBuilder
  public method Replace(a: string, b: string): StringBuilder
    return this;
  end Replace;
end StringBuilder;

public class Program
  public static method Main()
    var sb := new StringBuilder();
    _ := sb.Replace();
  end Main;
end Program;
