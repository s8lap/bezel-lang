//! case: s35_4_0002
//! doc: spec
//! section: 35.4
//! title: owning choice local-to-local assignment moves, unmarked
//! rule: Owning choices are move-only value types: local-to-local assignment moves
//!   (unmarked — copies are impossible, so there is nothing for a marker to
//!   disambiguate).
//! expect: run
//! stdout: widget disposed
module Conf;

public class Widget
  destructor
    Console.WriteLine("widget disposed");
  end destructor;
end Widget;

public choice Holder
  Full(owned w: Widget),
  Empty;
end Holder;

public class Program
  public static method Main()
    var a := Holder.Full(new Widget());
    using var b := a;          // unmarked local-to-local assignment moves; `a` is moved-from
    Console.WriteLine("before drop");
  end Main;
end Program;
