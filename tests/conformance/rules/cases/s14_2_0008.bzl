//! case: s14_2_0008
//! doc: spec
//! section: 14.2
//! title: a class not implementing the named interface fails where T: IFoo
//! rule: `where T: IFoo` admits only types conforming to `IFoo` per the
//!   interface-conformance rule; a non-conforming class is excluded (§14.2).
//! cites: 14.2
//! expect: reject
//! diagnostic: IFoo
//! issue: 4367
module Conf;

public interface IFoo
  method Ping();
end IFoo;

public class Box<T> where T: IFoo
  public constructor()
  end constructor;
end Box;

public class Other
  public constructor()
  end constructor;
end Other;

public class Holder
  private _b: Box<Other>?;

  public constructor()
  end constructor;
end Holder;
