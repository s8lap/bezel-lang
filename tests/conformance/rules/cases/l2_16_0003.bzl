//! case: l2_16_0003
//! doc: stdlib
//! section: 2.16
//! title: Weak<T>.TryUpgrade succeeds while the value is still alive
//! rule: TryUpgrade returns a new strong handle if the value is still alive,
//!   null otherwise (Try shape (b)).
//! cites: 2.16
//! expect: run
//! stdout: True
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var s := new Shared<Widget>(new Widget());
    using var w := s.Downgrade();
    using var s2 := w.TryUpgrade();
    Console.WriteLine(s2 <> null);
  end Main;
end Program;
