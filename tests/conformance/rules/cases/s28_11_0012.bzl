//! case: s28_11_0012
//! doc: spec
//! section: 28.11
//! title: a RemoveAt at the borrow's index after an intervening Insert draws no warning
//! rule: `RemoveAt(i)` where the borrow was taken as `coll[i]` with the same
//!   index expression — a constant, or a local read not reassigned between —
//!   and no structural operation of §29.12's list on the
//!   same collection binding between the borrow and the kill: a reposition or
//!   removal ahead of the slot would change which element the index names.
//! cites: 28.11, 29.12, 28.2
//! expect: quiet
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Ping()
  end Ping;

  destructor
  end destructor;
end Widget;

public class Program
  public method Demo()
    using var xs := new OwningArray<Widget>();
    xs.Add(new Widget());
    var w := xs[0];
    xs.Insert(0, new Widget());
    xs.RemoveAt(0);
    w.Ping();
  end Demo;
end Program;
