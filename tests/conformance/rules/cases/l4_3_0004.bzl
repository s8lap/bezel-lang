//! case: l4_3_0004
//! doc: stdlib
//! section: 4.3
//! title: OwningArray<T> conforms to IOwningList<T>, its own mutating family
//! rule: The mutating interfaces do not cross families; OwningArray<T>
//!   does implement IOwningList<T>, its own family's mutable abstraction.
//! cites: 4.3, 4.2
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public method AcceptOwningList<T>(items: IOwningList<T>) where T: class
end AcceptOwningList;

public class Program
  public method Demo()
    using var pool := new OwningArray<Widget>();
    AcceptOwningList(pool);
  end Demo;
end Program;
