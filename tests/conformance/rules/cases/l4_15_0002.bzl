//! case: l4_15_0002
//! doc: stdlib
//! section: 4.15
//! title: writing through an ArraySpan<T> stores a borrow with no disposal of the previous slot
//! rule: For class T, the assigned value is a borrow -- no ownership
//!   transfer; the previous slot contents are not disposed (they were
//!   borrows, not owned values). This differs from Span<T>'s class-T setter.
//! cites: 4.15
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
    using var w := new Widget();
    using var arr: Array<Widget> := [w];
    var span := arr.AsSpan();
    span[0] := w;
    Console.WriteLine("still alive");
  end Main;
end Program;
