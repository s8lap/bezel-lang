//! case: s29_2_0003
//! doc: spec
//! section: 29.2
//! title: the for variable over class-typed elements is a borrow of the slot
//! rule: What the `for` variable is, decided by the static type of the
//!   source: class elements, either family, default iteration, are a borrow
//!   of the slot.
//! cites: 29.2
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
