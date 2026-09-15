//! case: s26_3_0002
//! doc: spec
//! section: 26.3
//! title: an extension property must be expression-bodied
//! rule: An extension property is expression-bodied only; the block-accessor form
//!   is not admitted — multi-statement logic goes in a method.
//! cites: 26.3, 23.3
//! expect: reject
//! diagnostic: expression-bodied
module Conf;

public class Widget
  public property Size: int;

  public constructor(size: int)
    Size := size;
  end constructor;
end Widget;

extension Widget
  public property Doubled: int
    get => this.Size * 2;
  end Doubled;
end;
