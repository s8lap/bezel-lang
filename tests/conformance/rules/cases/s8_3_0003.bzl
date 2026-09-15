//! case: s8_3_0003
//! doc: spec
//! section: 8.3
//! title: the default Equals for a class is reference equality
//! rule: The compiler generates a default `Equals` appropriate to each type
//!   kind; for a class the default is reference equality.
//! cites: 8.3, 8.4
//! expect: run
//! stdout: False
module Conf;

public class Money
  private _cents: int;

  public constructor(cents: int)
    _cents := cents;
  end constructor;
end Money;

public class Program
  public static method Main()
    var a := new Money(5);
    var b := new Money(5);
    Console.WriteLine(a = b);
  end Main;
end Program;
