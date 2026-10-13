//! case: s9_2_0025
//! doc: spec
//! section: 9.2
//! title: a boxed record declaring only its own-typed Equals compares by that member
//! rule: On a record that is not auto-hashable, such a call reaches, member
//!   by member, the record's own declaration of `Equals` — the own-typed
//!   member, which is its `Object` slot ... a boxed `T` argument compares by
//!   it on the box's payload, any other argument, `null` included, unequal
//! cites: 9.2, 8.6
//! expect: run
//! output: True
//!   False
//!   False
//! issue: 5924
module Conf;

public record Money
  public readonly property Amount: int;
  public readonly property Note: string;

  public constructor(amount: int, note: string)
    Amount := amount;
    Note := note;
  end constructor;

  public method Equals(other: Money): bool => Amount = other.Amount;
  public method GetHashCode(): int => Amount;
end Money;

public class Program
  public static method Main()
    var o: Object := new Money(1, "x");
    Console.WriteLine(o.Equals(new Money(1, "y")));
    Console.WriteLine(o.Equals(new Money(2, "x")));
    Console.WriteLine(o.Equals(7));
  end Main;
end Program;
