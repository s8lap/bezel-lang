//! case: s26_3_0006
//! doc: spec
//! section: 26.3
//! title: an extension member may not be private
//! rule: Extension members must be declared `public` or `internal`; `protected` and
//!   `private` are compile errors — an extension block has no declaring class and
//!   no inheritance, so §13.1's definitions for those levels do not apply.
//! cites: 26.3, 13.1
//! expect: reject
//! diagnostic: private
module Conf;

public class Widget
  public property Size: int;

  public constructor(size: int)
    Size := size;
  end constructor;
end Widget;

extension Widget
  private method Hidden(): int => this.Size;
end;
