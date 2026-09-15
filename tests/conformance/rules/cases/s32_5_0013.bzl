//! case: s32_5_0013
//! doc: spec
//! section: 32.5
//! title: an owning inline array is not admitted at an extern parameter
//! rule: Owning inline arrays and inline arrays of other element types are not
//!   admitted.
//! cites: 32.5, 15.1.3
//! expect: reject
//! diagnostic: not admitted
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

extern "libexample"
  method StoreAll(items: [4 owned]Widget): int;
end;
