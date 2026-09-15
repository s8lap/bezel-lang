//! case: l4_9_0003
//! doc: stdlib
//! section: 4.9
//! title: OwningDictionary<K,V>.Remove disposes the removed value
//! rule: Remove(key: K): bool; // disposes the removed value.
//! cites: 4.9, 28.11
//! expect: run
//! stdout: widget disposed
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
    d.Remove("a");
    Console.WriteLine("after remove");
  end Main;
end Program;
