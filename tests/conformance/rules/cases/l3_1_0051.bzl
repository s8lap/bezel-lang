//! case: l3_1_0051
//! doc: stdlib
//! section: 3.1
//! title: the null-conditional spelling of a StringBuilder member call is rejected for the same arity reason
//! rule: stdlib §3.1 declares `Replace(oldValue: string, newValue: string): StringBuilder`
//!   - two parameters, neither carrying a default. §22.5: every parameter that
//!   declares no default must be bound, positionally or by name; leaving one
//!   unbound makes the candidate inapplicable, so §22.6's arity filtering rejects
//!   the call. The `?.` spelling of §7.5 decides whether the call happens on a
//!   null receiver; it does not change the member's declared arity.
//! cites: 3.1, 22.5, 22.6, 7.5
//! expect: reject
//! diagnostic: argument
//! issue: 7207
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder("hello");
    sb?.Replace();
  end Main;
end Program;
