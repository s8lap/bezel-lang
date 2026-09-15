//! case: l4_14_0007
//! doc: stdlib
//! section: 4.14
//! title: OwningBuffer<T>'s indexer setter disposes the slot's previous value at class T
//! rule: The indexer's setter takes ownership of the assigned value and
//!   disposes whatever the slot previously held.
//! cites: 4.14
//! expect: run
//! stdout: disposed
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("disposed");
  end destructor;
end Widget;

public class Program
  public static method Main()
    using var buf := new OwningBuffer<Widget>(1);
    buf[0] := new Widget();
    buf[0] := new Widget();
    Console.WriteLine("after");
  end Main;
end Program;
