//! case: s17_7_0004
//! doc: spec
//! section: 17.7
//! title: the new() shorthand is admitted at a typed field initializer
//! rule: "A field initializer or typed local declaration may use the `new()`
//!   shorthand when the type is unambiguous: `private owned _items:
//!   Array<int> := new();`"
//! cites: 17.7
//! expect: accept
module Conf;

public class Bucket
  private owned _items: Array<int> := new();

  public constructor()
  end constructor;
end Bucket;
