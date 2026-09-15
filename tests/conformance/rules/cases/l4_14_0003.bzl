//! case: l4_14_0003
//! doc: stdlib
//! section: 4.14
//! title: a Span<T> may not be constructed from an unbound temporary buffer
//! rule: A borrow-bearing record may not be constructed from a temporary
//!   bound to one of its borrow-carrying fields -- a fresh call result not
//!   bound to a named location has a lifetime ending at the construction
//!   expression, so the record would immediately outlive that source.
//! cites: 4.14, 9.7
//! expect: reject
//! diagnostic: temporary
module Conf;

public class Program
  public static method Main()
    var s := new Span<int>(new OwningBuffer<int>(3), 0, 3);
  end Main;
end Program;
