//! case: s26_1_0001
//! doc: spec
//! section: 26.1
//! title: an extension block adds a method and a computed property
//! rule: An extension block is declared at module level and adds methods and
//!   computed read-only properties to a type; inside the block `this` refers to
//!   the receiver.
//! cites: 26.1
//! expect: accept
module Conf;

public class Widget
  public property Size: int;

  public constructor(size: int)
    Size := size;
  end constructor;
end Widget;

extension Widget
  public property IsEmpty: bool => this.Size = 0;
  public method Doubled(): int => this.Size * 2;
end;
