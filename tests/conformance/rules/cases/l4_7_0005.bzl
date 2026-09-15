//! case: l4_7_0005
//! doc: stdlib
//! section: 4.7
//! title: draining a borrowing source into an owning list, one element at a time
//! rule: Build an owning list from owned elements ... or add them one at a
//!   time, taking them out of the source as you go: for var c in
//!   src.Drain() do dst.AddLast(move c); end;
//! cites: 4.7, 28.11
//! expect: run
//! stdout: 0
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var src := new OwningArray<Widget>();
    src.Add(new Widget());
    using var dst := new OwningLinkedList<Widget>();
    for var c in src.Drain() do
      dst.AddLast(move c);
    end;
    Console.WriteLine(src.Count);
  end Main;
end Program;
