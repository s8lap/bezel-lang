//! case: s25_2_0009
//! doc: spec
//! section: 25.2
//! title: a binary overload may carry the declaring type in either position
//! rule: A binary overload may pair the declaring type with any other type in
//!   either position.
//! cites: 25.2
//! expect: accept
module Conf;

public record Offset(Days: int)
  public static method operator +(n: int, o: Offset): Offset =>
    new Offset(n + o.Days);
end Offset;
