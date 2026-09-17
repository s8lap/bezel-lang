//! case: l4_15_0003
//! doc: stdlib
//! section: 4.15
//! title: an ArraySpan<T> may not be constructed from an unbound temporary array
//! rule: A borrow-bearing record may not be constructed from a temporary
//!   bound to one of its borrow-carrying fields; ArraySpan<T>'s Source field
//!   carries a borrow of the Array<T>, so a fresh unbound array is rejected.
//! cites: 4.15, 9.7
//! expect: reject
//! diagnostic: temporary
module Conf;

public class Program
  public static method Main()
    var s := new ArraySpan<int>([1, 2, 3], 0, 3);
  end Main;
end Program;
