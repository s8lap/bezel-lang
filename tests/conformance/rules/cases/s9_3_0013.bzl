//! case: s9_3_0013
//! doc: spec
//! section: 9.3
//! title: a positional record body auto-property with no initializer is rejected
//! rule: so a non-defaultable body field or auto-property is satisfied only by a field initializer ([§22.8](#sec-field-initializers)); a body constructor, of any parameter list, does not assign it for the primary.
//! cites: 9.3, 7.9, 22.8, 23.1
//! expect: reject
//! diagnostic: needs an initializer
//! issue: 8097
module Conf;

public record Holder(Seed: int)
  public readonly property C: string;

  public constructor(seed: int, c: string)
    Seed := seed;
    C := c;
  end constructor;
end Holder;
