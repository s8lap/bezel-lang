//! case: l4_5_0010
//! doc: stdlib
//! section: 4.5
//! title: move arr[i] takes ownership and leaves the slot moved-from
//! rule: To take an element out, use PopAt(i) ... or move arr[i] (returns
//!   owned T and leaves the slot in moved-from state; the caller must
//!   reassign before the next exit point).
//! cites: 4.5, 28.8.1, 28.11
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
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget());
    var taken := move pool[0];
    pool[0] := new Widget();
    Console.WriteLine("still alive");
    taken.Dispose();
  end Main;
end Program;
