//! case: s8_1_2002
//! doc: spec
//! section: 8.1
//! title: calling ToString with a format argument on a class that declares
//!   only Object's ToString is rejected
//! rule: public virtual method ToString(): string;
//! cites: 12.2, 18.5
//! expect: reject
//! diagnostic: formattable
module Conf;

class P
  public constructor()
  end constructor;
end P;

public class Program
  public static method Main()
    using var p := new P();
    Console.WriteLine(p.ToString("x"));
  end Main;
end Program;
