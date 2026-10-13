//! case: s9_2_0028
//! doc: spec
//! section: 9.2
//! title: a record declaring GetHashCode and no Equals admits a direct GetHashCode call
//! rule: Calling `someRecord.Equals(other)` or `someRecord.GetHashCode()` on a
//!   non-auto-hashable record without a user-defined declaration of that
//!   member — the `override` spelling or the plain one — is a compile error
//!   ... The error is per member
//! cites: 9.2
//! expect: run
//! stdout: 5
module Conf;

public record Money
  public readonly property Amount: int;

  public constructor(amount: int)
    Amount := amount;
  end constructor;

  public method GetHashCode(): int => Amount;
end Money;

public class Program
  public static method Main()
    Console.WriteLine(new Money(5).GetHashCode());
  end Main;
end Program;
