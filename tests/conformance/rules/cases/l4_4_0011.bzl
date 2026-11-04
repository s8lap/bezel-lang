//! case: l4_4_0011
//! doc: stdlib
//! section: 4.4
//! title: a fresh producer stored via Array<T>.Add is rejected at the store site
//! rule: A fresh unbound owned temporary of class type ... in a single-element store
//!   position of a borrowing-family collection is a compile error at the store
//!   site (§15.4, v0.355; the stdlib §4.4 store positions are borrowing).
//! cites: 4.4, 15.4, 28.11
//! expect: reject
//! diagnostic: §15.4 fresh-store error
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
    using var arr := new Array<Widget>();
    arr.Add(new Widget());
  end Fill;
end Program;
