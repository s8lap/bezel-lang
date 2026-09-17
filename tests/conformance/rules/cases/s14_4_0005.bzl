//! case: s14_4_0005
//! doc: spec
//! section: 14.4
//! title: a value type argument is rejected on OwningTask<T>
//! rule: `OwningTask<T>` is a compile error when T's underlying
//!   (nullability-stripped) type is neither a class nor an interface type
//!   (§14.4, Appendix C.6).
//! cites: 14.4
//! expect: reject
//! diagnostic: class
//! issue: 4367
module Conf;

public class Holder
  private _t: OwningTask<int>?;

  public constructor()
  end constructor;
end Holder;
