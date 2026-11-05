//! case: s7_1_2101
//! doc: spec
//! section: 7.1
//! title: a null literal cannot initialise a non-nullable TextWriter type
//!   argument
//! rule: The compiler enforces the distinction at every assignment,
//!   parameter pass, and return.
//! cites: 7.3, 14.1
//! expect: reject
//! diagnostic: nullable
//! notes: Green on base and on the staging tip: the over-admission control
//!   for the #7792 unit (a TextWriter value stays non-nullable through a
//!   generic parameter).
module Conf;

public class Box<T>
  public v: T;

  public constructor(x: T)
    v := x;
  end constructor;
end Box;

public class Program
  public static method Main()
    var b := new Box<TextWriter>(null);
    Console.WriteLine("end");
  end Main;
end Program;
