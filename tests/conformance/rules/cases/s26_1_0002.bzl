//! case: s26_1_0002
//! doc: spec
//! section: 26.1
//! title: an extension block may close with end extension
//! rule: The block ends with `end;` or `end extension;`.
//! cites: 26.1, 1.6
//! expect: accept
module Conf;

public class Widget
  public property Size: int;

  public constructor(size: int)
    Size := size;
  end constructor;
end Widget;

public extension Widget
  public method Doubled(): int => this.Size * 2;
end extension;
