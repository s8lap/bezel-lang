//! case: l4_4_0012
//! doc: stdlib
//! section: 4.4
//! title: a fresh non-trivial-dispose element inside an Array<T> literal is a hard error
//! rule: For borrowing-family targets, an element expression that is a
//!   fresh producer of non-trivial-dispose class type is rejected at the
//!   literal site — the temporary would be disposed at the end of the
//!   declaration statement, leaving the collection holding a dangling
//!   borrow from birth.
//! cites: 4.4, 15.4, 28.11
//! expect: reject
//! diagnostic: literal
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
    using var arr: Array<Widget> := [new Widget()];
  end Fill;
end Program;
