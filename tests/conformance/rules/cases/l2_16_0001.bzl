//! case: l2_16_0001
//! doc: stdlib
//! section: 2.16
//! title: disposing a cloned handle does not dispose the underlying value
//! rule: N handles jointly own one underlying value; the last handle's
//!   disposal disposes it — Clone() increments the count, and disposing one
//!   handle while another lives leaves the value alive.
//! cites: 2.16, 28.13
//! expect: run
//! stdout: still alive
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("widget disposed");
  end destructor;
end Widget;

public class Program
  public static method Main()
    using var s1 := new Shared<Widget>(new Widget());
    do
      using var s2 := s1.Clone();
    end;
    Console.WriteLine("still alive");
  end Main;
end Program;
