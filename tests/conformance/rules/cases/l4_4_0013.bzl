//! case: l4_4_0013
//! doc: stdlib
//! section: 4.4
//! title: a bound source in an Array<T> literal is admitted, unlike a fresh producer
//! rule: The remedy for a fresh producer inside a borrowing-family literal
//!   names the source first: bind the source to a named holder, whose
//!   borrow is then admitted at the literal site.
//! cites: 4.4, 15.4, 28.11
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("widget disposed");
  end destructor;
end Widget;

public class Program
  public method Fill()
    using var w := new Widget();
    using var arr: Array<Widget> := [w];
  end Fill;
end Program;
