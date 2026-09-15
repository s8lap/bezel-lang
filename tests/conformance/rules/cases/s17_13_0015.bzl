//! case: s17_13_0015
//! doc: spec
//! section: 17.13
//! title: a binding-atom clause named _ is a compile error
//! rule: "a binding-atom clause (`(var _ := E; body)`), whose remedy is
//!   naming the binding or restructuring the expression — the statement
//!   remedies above are not spellable inside the atom."
//! cites: 17.13, 17.15
//! expect: reject
//! diagnostic: discard
module Conf;

public class Program
  public static method Main()
    var r := (var _ := 5; _ + 1);
  end Main;
end Program;
