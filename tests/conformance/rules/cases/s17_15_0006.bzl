//! case: s17_15_0006
//! doc: spec
//! section: 17.15
//! title: a borrow sourced from an atom binding may not escape the atom
//! rule: "A borrow whose source is an atom binding may not escape the atom
//!   — in the body's value or any other holder — per K8.4: rejected, never
//!   lifetime-extended; bind at statement level (`using var`) when the
//!   value must outlive the expression."
//! cites: 17.15, 28.3
//! expect: reject
//! diagnostic: escape the atom
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    var r := (var w := new Widget(); w);
  end Main;
end Program;
