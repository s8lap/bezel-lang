//! case: l4_9_0005
//! doc: stdlib
//! section: 4.9
//! title: PopValue transfers ownership out without disposing
//! rule: PopValue(key: K): owned V; // raises if not present — the take
//!   form, in contrast with GetOrNull's look.
//! cites: 4.9, 28.11
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
    using var d := new OwningDictionary<string, Widget>();
    d.Add("a", new Widget());
    using var w := d.PopValue("a");
    Console.WriteLine("still alive");
  end Main;
end Program;
