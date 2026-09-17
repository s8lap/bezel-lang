//! case: s28_3_0037
//! doc: spec
//! section: 28.3
//! title: a discard while a borrow is live is not frozen
//! rule: Site-announced kills are unfrozen: the discard `_ := x;` is permitted while
//!   borrows of the value live.
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    var w := new Widget();
    var b := w;
    _ := w;
    var name := b.ToString();
  end Demo;
end Program;
