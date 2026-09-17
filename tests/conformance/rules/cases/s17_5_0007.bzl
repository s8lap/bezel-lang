//! case: s17_5_0007
//! doc: spec
//! section: 17.5
//! title: the Contains lookup is structural — a user type's own Contains satisfies in
//! rule: "The `Contains` lookup is structural rather than interface-based: any
//!   accessible method named `Contains` taking one parameter of compatible
//!   type and returning `bool` qualifies, whether it is an instance member or
//!   an extension method."
//! cites: 17.5, 26
//! expect: run
//! stdout: True
module Conf;

public class Bag
  private readonly _limit: int;

  public constructor(limit: int)
    _limit := limit;
  end constructor;

  public method Contains(item: int): bool => item <= _limit;
end Bag;

public class Program
  public static method Main()
    using var bag := new Bag(10);
    var r := 5 in bag;
    Console.WriteLine(r);
  end Main;
end Program;
