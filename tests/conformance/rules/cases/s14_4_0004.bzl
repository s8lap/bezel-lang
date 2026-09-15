//! case: s14_4_0004
//! doc: spec
//! section: 14.4
//! title: a nullable class type element is admitted on OwningArray<T>
//! rule: The owning-type instantiation rule checks the underlying type, not
//!   the nullable wrapper; a nullable class type is admitted (§14.4).
//! cites: 14.4
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Holder
  private _p: OwningArray<Widget?>?;

  public constructor()
  end constructor;
end Holder;
