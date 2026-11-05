//! case: s8_1_2003
//! doc: spec
//! section: 8.1
//! title: a class declaring ToString(format) without IFormattable accepts
//!   the call p.ToString with a format
//! rule: a class may declare a non-`override` instance member named
//!   `Equals`, `GetHashCode` or `ToString` at any other parameter list
//! cites: 12.2, 18.5
//! expect: run
//! stdout: P:x
module Conf;

class P
  public constructor()
  end constructor;

  public method ToString(format: string): string
    return "P:" + format;
  end ToString;
end P;

public class Program
  public static method Main()
    using var p := new P();
    Console.WriteLine(p.ToString("x"));
  end Main;
end Program;
