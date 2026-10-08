//! case: s28_10_0014
//! doc: spec
//! section: 28.10
//! title: the marked-arm remedy (if c then move a else move b) adopts and is admitted
//! rule: The remedies are the marked arms where the arm position admits the
//!   marker — `(if c then move a else move b)`, an expression-`case`
//!   likewise — except at an arm naming a `using var` source or the `case`
//!   scrutinee's own binding.
//! cites: 28.10, 28.3, 20
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Ping()
  end Ping;
end Widget;

public class Program
  public method Run(c: bool)
    var a := new Widget();
    var b := new Widget();
    using var held := (if c then move a else move b);
    held.Ping();
  end Run;
end Program;
