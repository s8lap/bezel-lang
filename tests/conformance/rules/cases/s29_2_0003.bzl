//! case: s29_2_0003
//! doc: spec
//! section: 29.2
//! title: the for variable over class-typed elements is a borrow of the slot
//! rule: For a borrow slot — a class-typed element of a borrowing collection —
//!   the element operation *indexer-get / default iteration* is "borrow out"
//!   (§28.11's element slot policy), so the loop variable names the same object
//!   the slot does and a mutation through it is visible in the collection.
//! cites: 29.2, 28.11
//! expect: run
//! stdout: a!
module Conf;

public class Widget
  public property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;
end Widget;

public class Program
  public static method Main()
    var widgets: Array<Widget> := [new Widget("a"), new Widget("b")];
    for var w in widgets do
      w.Name := $"{w.Name}!";
    end;
    Console.WriteLine(widgets[0].Name);
  end Main;
end Program;
