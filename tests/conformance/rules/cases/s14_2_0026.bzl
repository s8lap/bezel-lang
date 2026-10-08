//! case: s14_2_0026
//! doc: spec
//! section: 14.2
//! title: the same call on a receiver typed at the base class stays an ordinary generic call
//! rule: a call on a receiver typed at the base class that declares the
//!   witness as an ordinary, non-`virtual` generic method names no dispatched
//!   declaration and stays an ordinary generic call, checked per
//!   instantiation
//! cites: 14.2, 3.6
//! expect: accept
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
  public static method Run(b: B)
    b.M<[2 owned]Node>();
  end Run;
end Caller;
