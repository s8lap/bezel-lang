//! case: l4_9_0002
//! doc: stdlib
//! section: 4.9
//! title: OwningDictionary<K,V>.TryAdd disposes the passed value on a duplicate key
//! rule: TryAdd(key: K, owned value: V): bool; // v0.282: false on
//!   duplicate, and the passed value is disposed on that path.
//! cites: 4.9, 1
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
    var ok := d.TryAdd("a", new Widget());
    Console.WriteLine(ok);
  end Main;
end Program;
