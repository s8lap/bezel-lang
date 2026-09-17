//! case: s22_6_0013
//! doc: spec
//! section: 22.6
//! title: an unmarked borrow read selects the plain member of an owned-ordinary pair
//! rule: "A borrow is applicable only to the plain candidate: an unmarked
//!   read of a named binding ... `r.Append(w)`; // borrow — the plain
//!   overload; w untouched."
//! cites: 22.6
//! expect: run
//! stdout: borrow
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Registry
  public constructor()
  end constructor;

  public method Append(item: Widget): string => "borrow";
  public method Append(owned item: Widget): string => "take";
end Registry;

public class Program
  public static method Main()
    using var r := new Registry();
    using var w := new Widget();
    Console.WriteLine(r.Append(w));
  end Main;
end Program;
