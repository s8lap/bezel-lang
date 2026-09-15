//! case: l4_16_0004
//! doc: stdlib
//! section: 4.16
//! title: an IReadOnlySpan<TElement> parameter may not be stored in a field that outlives the call
//! rule: A method declaring IReadOnlySpan<TElement> cannot store the
//!   parameter in a field that outlives the call -- the standard
//!   borrow-bearing-record constraints; nothing new is added by the interface.
//! cites: 4.16, 9.7
//! expect: reject
//! diagnostic: field
module Conf;

public class Holder
  private _s: IReadOnlySpan<int>?;

  public constructor()
  end constructor;

  public method Remember(s: IReadOnlySpan<int>)
    _s := s;
  end Remember;
end Holder;
