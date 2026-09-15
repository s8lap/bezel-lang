//! case: s26_3_0007
//! doc: spec
//! section: 26.3
//! title: an extension member may not be virtual
//! rule: An extension block declares no `virtual`, `override`, `abstract` or
//!   `sealed` members — extensions are statically dispatched.
//! cites: 26.3, 26.5
//! expect: reject
//! diagnostic: virtual
module Conf;

public class Widget
  public property Size: int;

  public constructor(size: int)
    Size := size;
  end constructor;
end Widget;

extension Widget
  public virtual method Doubled(): int => this.Size * 2;
end;
