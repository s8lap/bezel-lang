//! case: l9_6_0022
//! doc: stdlib
//! section: 9.6
//! title: a call to PadCenter supplying three arguments matches no declared form
//! rule: public pure method PadCenter(totalWidth: int, paddingChar: char := ' '): string;
//!   — the declared form takes one or two arguments, the second declaring a
//!   default, so a call supplying three binds no declared form; arity filtering
//!   leaves no candidate and the call is a compile error.
//! cites: 9.6, 22.5, 22.6
//! expect: reject
//! diagnostic: PadCenter
module Conf;

public class Program
  public static method Main()
    var a := "ab".PadCenter(5, 'x', 'y');
    Console.WriteLine(a.Length);
  end Main;
end Program;
