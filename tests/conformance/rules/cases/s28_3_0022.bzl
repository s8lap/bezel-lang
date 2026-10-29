//! case: s28_3_0022
//! doc: spec
//! section: 28.3
//! title: using var over a borrowing alias is a borrow-pin that assumes no obligation
//! rule: An unmarked `using var y := a;` where `a` is a borrow-state named binding
//!   (here a borrowing alias `var a := w;`) does not assume a disposal
//!   obligation: the owner `w` keeps it and stays readable, and the pin extends
//!   the source's required liveness to the block's end (§28.10 v0.350, "`using
//!   var` initialized from a borrow": an unmarked read of a **borrow-state**
//!   named binding — a plain parameter, a borrowing alias or loop variable, a
//!   captured owned binding inside a closure). The unmarked `using var pin := w;`
//!   directly over the owned `w` is a compile error since v0.350 (§28.10
//!   *Adopting a named owned binding*; case s28_10_0015), so the pin is taken
//!   through the alias.
//! cites: 28.10
//! expect: run
//! output: pinned
//!   disposed
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Ping()
    Console.WriteLine("pinned");
  end Ping;

  destructor
    Console.WriteLine("disposed");
  end destructor;
end Widget;

public class Program
  public static method Main()
    using var w := new Widget();
    var a := w;
    using var pin := a;
    w.Ping();
  end Main;
end Program;
