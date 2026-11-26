//! case: s26_7_3603
//! doc: spec
//! section: 26.7
//! title: an extension over Object storing this into a field stays under the ban
//! rule: an extension over `Object` or an interface is under the first sentence whatever its receiver
//!   holds at run time, a value of such a declared type being classified by its form, a borrow
//! cites: 26.7, 28.3
//! expect: reject
//! diagnostic: this
//! issue: 8113
//! notes: Contrast s26_7_3601, where the head is `string`.
module Conf;

public class Holder
  public V: Object? := null;
  public constructor() end constructor;
end Holder;

public extension Object
  public method Keep(h: Holder)
    h.V := this;
  end Keep;
end extension;
