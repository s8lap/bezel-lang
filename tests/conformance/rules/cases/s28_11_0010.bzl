//! case: s28_11_0010
//! doc: spec
//! section: 28.11
//! title: an element borrow re-taken after the mutation is clean
//! rule: A borrow re-taken after the mutation is clean, and a use before the
//!   mutation, or on a path the kill does not dominate, draws nothing.
//! cites: 28.11, 28.2
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
    xs.Add(new Widget());
    var first := xs[0];
    first.Ping();
    xs.RemoveAt(0);
    var again := xs[0];
    again.Ping();
  end Demo;
end Program;
