//! case: s29_2_0010
//! doc: spec
//! section: 29.2
//! title: an undischarged owned Drain() iteration variable auto-drops with a per-iteration must-use warning
//! rule: The compiler applies the standard §28.3 owned-local discharge rule to
//!   the iteration variable on the loop body's every exit edge: on each
//!   normal-exit edge `x` is either discharged by the program or auto-dropped
//!   with the must-use warning, and the fall-through-to-the-next-iteration edge
//!   is checked the same way as the body's end.
//! cites: 29.2, 28.3
//! expect: warn
//! diagnostic: must use
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("gone");
  end destructor;
end Widget;

public class Program
  public static method Main()
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget());
    for var conn in pool.Drain() do
      Console.WriteLine("iter");
    end;
  end Main;
end Program;
