//! case: s17_13_0007
//! doc: spec
//! section: 17.13
//! title: a read after a conditionally-discarded binding is a compile error at the merge
//! rule: "`_ := x;` ... `x` is thereafter treated exactly as explicitly
//!   disposed ... and a join with a still-owned path yields maybe-consumed."
//!   §28.2: "at a join of an explicitly-disposed path with a still-owned path
//!   the variable is **maybe-consumed** and a later use is a compile error."
//! cites: 17.13, 28.2, 28.3
//! expect: reject
//! diagnostic: maybe-consumed
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
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
