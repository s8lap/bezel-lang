//! case: s22_1_0002
//! doc: spec
//! section: 22.1
//! title: a procedure omits the colon return-type clause entirely
//! rule: A method has a return type after `:`, or no `:` clause for a
//!   procedure (return type `()`).
//! cites: 22.1
//! expect: accept
module Conf;

public class Printer
  public constructor()
  end constructor;

  public method PrintGreeting(name: string)
    Console.WriteLine($"Hello, {name}!");
  end PrintGreeting;
end Printer;
