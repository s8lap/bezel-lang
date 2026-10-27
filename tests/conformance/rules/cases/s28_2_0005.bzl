//! case: s28_2_0005
//! doc: spec
//! section: 28.2
//! title: a conditional dispose joins to maybe-consumed and a later read is rejected
//! rule: A conditional kill of the variable: at a join of an
//!   explicitly-disposed path with a still-owned path the variable is
//!   maybe-consumed and a later use is a compile error.
//! expect: reject
//! diagnostic: consumed
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo(flag: bool)
    var w := new Widget();
    if flag then
      w.Dispose();
    end;
    var text := w.ToString();
  end Demo;
end Program;
