//! case: s29_11_0009
//! doc: spec
//! section: 29.11
//! title: a class-element terminal accessor over a concrete collection returns a live element borrow
//! rule: A **concrete collection value** as the root — a `new` collection, or a
//!   collection-typed parameter, field, or local bound to one — is a silent
//!   root of the terminal-accessor provenance predicate, and soundly so: the
//!   elements' owner is the collection, which outlives the accessor call, so
//!   the returned element borrow is live after the call returns.
//! cites: 29.11, 28.11
//! expect: run
//! stdout: b
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
    var w := widgets.Last();
    Console.WriteLine(w.Name);
  end Main;
end Program;
