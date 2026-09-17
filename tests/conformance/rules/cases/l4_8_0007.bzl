//! case: l4_8_0007
//! doc: stdlib
//! section: 4.8
//! title: Dictionary<K,V>.Clear drops every entry without disposing keys or values
//! rule: Clear(); // drops every entry; disposes neither keys and values.
//! cites: 4.8, 28.11
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
    using var d := new Dictionary<string, Widget>();
    d.Add("a", w);
    d.Clear();
    Console.WriteLine("still alive");
  end Main;
end Program;
