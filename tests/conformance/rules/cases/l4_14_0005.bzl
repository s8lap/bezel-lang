//! case: l4_14_0005
//! doc: stdlib
//! section: 4.14
//! title: a Span<T> may not be stored in an instance field of a class
//! rule: A borrow-bearing record value may not be stored in an instance
//!   field of a class -- a class instance's lifetime is not bounded by any
//!   borrow source's scope, and the store is rejected statically.
//! cites: 4.14, 9.7
//! expect: reject
//! diagnostic: instance field
module Conf;

public class Holder
  private _s: Span<int>;

  public constructor(s: Span<int>)
    _s := s;
  end constructor;
end Holder;
