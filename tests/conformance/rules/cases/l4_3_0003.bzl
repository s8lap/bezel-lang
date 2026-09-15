//! case: l4_3_0003
//! doc: stdlib
//! section: 4.3
//! title: IList<T> and IOwningList<T> are unrelated types
//! rule: IList<T> (borrowing) and IOwningList<T> (owning) are unrelated
//!   types with no subtyping; code that abstracts over "any mutable list"
//!   must pick one.
//! cites: 4.3
//! expect: reject
//! diagnostic: IList
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public method AcceptList<T>(items: IList<T>)
end AcceptList;

public class Program
  public method Demo()
    using var pool := new OwningArray<Widget>();
    AcceptList(pool);
  end Demo;
end Program;
