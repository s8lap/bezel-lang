//! case: l4_9_0004
//! doc: stdlib
//! section: 4.9
//! title: OwningDictionary<K,V>.Clear disposes every value
//! rule: Clear(); // disposes every value — the dictionary's Dispose()
//!   cascade handles the same disposal.
//! cites: 4.9, 28.11
//! expect: run
//! output: widget disposed
//!   widget disposed
//!   after clear
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
    d.Add("b", new Widget());
    d.Clear();
    Console.WriteLine("after clear");
  end Main;
end Program;
