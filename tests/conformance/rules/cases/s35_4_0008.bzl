//! case: s35_4_0008
//! doc: spec
//! section: 35.4
//! title: using var over an owning choice suppresses the must-use warning
//! rule: `using var` is admitted over an owning choice — it locks reassignment
//!   and suppresses the must-use warning on that auto-drop, the auto-dispose
//!   running either way.
//! cites: 15.3, 28.10, 28.3
//! expect: quiet
module Conf;

public class Resource
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("disposed");
  end destructor;
end Resource;

public choice Holding
  Held(owned res: Resource),
  Nothing;
end Holding;

public class Program
  public static method Use()
    using var h := Holding.Held(new Resource());
  end Use;
end Program;
