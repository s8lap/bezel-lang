//! case: l2_8_0002
//! doc: stdlib
//! section: 2.8
//! title: a class implementing CompareTo satisfies IComparable<T>
//! rule: IComparable<T> declares method CompareTo(other: T): int; a class
//!   supplying it satisfies the interface's inheritance-clause declaration.
//! expect: accept
module Conf;

public class Money: IComparable<Money>
  public readonly property Amount: int;

  public constructor(amount: int)
    Amount := amount;
  end constructor;

  public method CompareTo(other: Money): int =>
    (if Amount < other.Amount then -1 elif Amount > other.Amount then 1 else 0);
end Money;
