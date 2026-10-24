//! case: l9_6_0018
//! doc: stdlib
//! section: 9.6
//! title: a call to TrimStart supplying an argument matches no declared form
//! rule: public pure method TrimStart(): string; — the declared form takes no
//!   argument, and every parameter that declares no default must be bound;
//!   a call supplying an argument binds no declared form and is a compile
//!   error.
//! cites: 9.6, 22.5, 22.6
//! expect: reject
//! diagnostic: TrimStart
module Conf;

public class Program
  public static method Main(): int
    var t := " abc ".TrimStart(' ');
    Console.WriteLine(t.Length);
    return 0;
  end Main;
end Program;
