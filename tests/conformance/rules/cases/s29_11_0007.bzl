//! case: s29_11_0007
//! doc: spec
//! section: 29.11
//! title: a class-element terminal accessor over a minting same-unit iterator draws the provenance warning
//! rule: A call [to a class-element terminal accessor] must draw a warning
//!   iff the receiver's statically-tracked provenance ... roots at a
//!   directly-called iterator method, compiled in the same compilation
//!   unit, at least one of whose reachable `yield return` sites classifies
//!   as an enumerator-owned yield.
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
    var w := Widgets().First();
    Console.WriteLine(w.Name);
  end Main;
end Program;
