//! case: s4_5_0002
//! doc: spec
//! section: 4.5
//! title: a surrogate code point is not a valid char value
//! rule: Surrogate code points (U+D800 to U+DFFF) are not valid char values.
//! cites: 4.5
//! expect: reject
//! diagnostic: surrogate
module Conf;

public class Holder
  public method Compute()
    var c: char := '\uD800';
  end Compute;
end Holder;
