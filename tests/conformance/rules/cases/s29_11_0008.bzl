//! case: s29_11_0008
//! doc: spec
//! section: 29.11
//! title: the terminal-accessor provenance warning traces through a materializer pass-through node
//! rule: At a class or interface element type the trace does not stop at a
//!   materializer result — `ToArray()`/`ToSet()`/`ToDictionary(…)` ... They
//!   are pass-through nodes, never roots: the verdict is always the root's.
//! cites: 29.11
//! expect: warn
//! diagnostic: provenance
module Conf;

public class Widget
  public property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;
end Widget;

public class Program
  public static method Widgets(): IEnumerable<Widget>
    yield return new Widget("a");
    yield return new Widget("b");
  end Widgets;

  public static method Main()
    var w := Widgets().ToArray().First();
    Console.WriteLine(w.Name);
  end Main;
end Program;
