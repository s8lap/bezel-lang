//! case: s8_1_2001
//! doc: spec
//! section: 8.1
//! title: ToString with a format argument on an enum value is rejected:
//!   only ToString() exists
//! rule: public virtual method ToString(): string;
//! cites: 10.5, 18.5
//! expect: reject
//! diagnostic: formattable
module Conf;

enum E
  A, B;
end E;

public class Program
  public static method Main()
    var e := E.A;
    Console.WriteLine(e.ToString("F4"));
  end Main;
end Program;
