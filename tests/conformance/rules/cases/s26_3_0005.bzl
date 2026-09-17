//! case: s26_3_0005
//! doc: spec
//! section: 26.3
//! title: a static member inside an extension block is rejected
//! rule: An extension block adds receiver-directed members only: a `static`
//!   modifier inside an extension block is a compile error.
//! cites: 26.3, 22.12
//! expect: reject
//! diagnostic: static
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

extension Widget
  public static method Origin(): int => 0;
end;
