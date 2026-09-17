//! case: l2_8_0001
//! doc: stdlib
//! section: 2.8
//! title: a class declaring IComparable<T> must supply CompareTo
//! rule: IComparable<T> declares method CompareTo(other: T): int; a class
//!   whose inheritance clause lists IComparable<T> must implement it.
//! expect: reject
//! diagnostic: CompareTo
module Conf;

public class Money: IComparable<Money>
  public readonly property Amount: int;

  public constructor(amount: int)
    Amount := amount;
  end constructor;
end Money;
