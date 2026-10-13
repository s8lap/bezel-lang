//! case: s9_2_0024
//! doc: spec
//! section: 9.2
//! title: a plain GetHashCode beside a plain own-typed Equals discharges the obligation on a record
//! rule: On a value kind — a record, an enum or a choice — whose
//!   `Object`-slot `Equals` takes its own type and is declared `override` or
//!   plain, the two spellings declaring one member ... the rule reads both
//!   spellings of both members: a plain `Equals` obliges `GetHashCode`, and
//!   a plain `GetHashCode` discharges the obligation.
//! cites: 9.2, 8.3
//! expect: run
//! output: True
//!   False
module Conf;

public record Money
  public readonly property Amount: int;

  public constructor(amount: int)
    Amount := amount;
  end constructor;

  public method Equals(other: Money): bool => Amount mod 10 = other.Amount mod 10;
  public method GetHashCode(): int => Amount mod 10;
end Money;

public class Program
  public static method Main()
    Console.WriteLine(new Money(1).Equals(new Money(11)));
    Console.WriteLine(new Money(1).Equals(new Money(2)));
  end Main;
end Program;
