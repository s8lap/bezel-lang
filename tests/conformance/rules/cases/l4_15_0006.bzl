//! case: l4_15_0006
//! doc: stdlib
//! section: 4.15
//! title: neither ArraySpan<T> nor ReadOnlyArraySpan<T> declares ToArray()
//! rule: No ToArray() on either span family -- neither materializes; a view
//!   on this side is over an Array<T>, which already carries ToArray() and
//!   Slice, so materialization is reachable through the source instead.
//! cites: 4.15
//! expect: reject
//! diagnostic: ToArray
module Conf;

public class Program
  public static method Main()
    using var xs: Array<int> := [1, 2, 3];
    var ro := xs.AsReadOnlySpan();
    using var copy := ro.ToArray();
  end Main;
end Program;
