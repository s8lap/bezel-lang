//! case: l3_1_0048
//! doc: stdlib
//! section: 3.1
//! title: a StringBuilder member call that supplies fewer arguments than the member declares is rejected
//! rule: stdlib §3.1 declares `Replace(oldValue: string, newValue: string): StringBuilder`
//!   - two parameters, neither carrying a default. §22.5: every parameter that
//!   declares no default must be bound, positionally or by name; leaving one
//!   unbound makes the candidate inapplicable, so §22.6's arity filtering rejects
//!   the call.
//! cites: 3.1, 22.5, 22.6
//! expect: reject
//! diagnostic: argument
//! issue: 7207
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder("hello");
    _ := sb.Replace();
  end Main;
end Program;
