//! case: s14_4_0006
//! doc: spec
//! section: 14.4
//! title: a nullable class type argument is admitted on OwningTask<T>
//! rule: Nullable class types are admitted for the owning task family: the
//!   underlying type is checked, and the nullability of the result is
//!   independent of the disposal obligation (§14.4).
//! cites: 14.4
//! expect: accept
//! issue: 4367
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Holder
  private _t: OwningTask<Widget?>?;

  public constructor()
  end constructor;
end Holder;
