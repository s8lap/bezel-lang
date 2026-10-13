//! case: s9_2_0026
//! doc: spec
//! section: 9.2
//! title: an Object?-argument Equals through a type parameter reaches a record's own-typed Equals
//! rule: At a record, an enum or a choice S, the first kind reaches S's
//!   own-typed `Equals`, declared or generated — its `Object` slot ...; the
//!   second kind reaches the same member through the slot: an argument that
//!   is a boxed S compares by it on the box's payload, and any other
//!   argument, `null` included, compares unequal
//! cites: 9.2, 14.2
//! expect: run
//! output: True
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
  public static method Same<T>(x: T, o: Object?): bool where T: notnull => x.Equals(o);

  public static method Main()
    var a := new Money(1, "x");
    var boxed: Object? := new Money(1, "y");
    Console.WriteLine(Same<Money>(a, boxed));
    Console.WriteLine(Same<Money>(a, "x"));
  end Main;
end Program;
