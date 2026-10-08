//! case: s14_2_0025
//! doc: spec
//! section: 14.2
//! title: a call on the conforming class of an inherited witness is excluded at an owning inline array
//! rule: The type parameters of a dispatched generic method admit no move-only
//!   value type (§15.3) and no inline array of any form (§15.1) as a type
//!   argument. ... a call on a receiver typed at a class for which the method
//!   is an inherited witness (*Constraints on overrides and interface
//!   implementations* above), or at a class deriving from it, is such a call
//! cites: 14.2, 11.2
//! expect: reject
//! diagnostic: inline array
//! issue: 5640
module Conf;

public class Node
  public constructor()
  end constructor;
end Node;

public interface IProbe
  method M<T>();
end IProbe;

public class B
  public constructor()
  end constructor;

  public method M<T>()
    Console.WriteLine("probe");
  end M;
end B;

public class D: B, IProbe
  public constructor()
  end constructor;
end D;

public class Caller
  public static method Run(d: D)
    d.M<[2 owned]Node>();
  end Run;
end Caller;
