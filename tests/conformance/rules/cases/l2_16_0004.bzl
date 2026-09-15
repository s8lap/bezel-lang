//! case: l2_16_0004
//! doc: stdlib
//! section: 2.16
//! title: Weak<T>.TryUpgrade fails once the value has been disposed
//! rule: TryUpgrade returns null once the underlying value is no longer
//!   alive — Weak<T> does not keep the value alive, and after the last
//!   Shared handle disposes it, upgrading fails.
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
    s.Dispose();
    using var s2 := w.TryUpgrade();
    Console.WriteLine(s2 = null);
  end Main;
end Program;
