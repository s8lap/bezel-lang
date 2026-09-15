//! case: s17_13_0007
//! doc: spec
//! section: 17.13
//! title: a read after a conditionally-discarded binding is a compile error at the merge
//! rule: "a read after a *conditional* discard is a compile error at the
//!   merge."
//! cites: 17.13, 28.3
//! expect: reject
//! diagnostic: consumed
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Demo(flag: bool)
    var w := new Widget();
    if flag then
      _ := w;
    end;
    var text := w.ToString();
  end Demo;
end Program;
