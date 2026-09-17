//! case: s35_4_0002
//! doc: spec
//! section: 35.4
//! title: owning choice local-to-local assignment moves, unmarked, and `using var` adopts
//! rule: Owning choices are move-only value types: local-to-local assignment moves
//!   (unmarked — copies are impossible, so there is nothing for a marker to
//!   disambiguate); `using var` is admitted with pin-only effect.
//! cites: 35.4, 15.3, 28.10
//! expect: run
//! output: inner
//!   widget disposed
//!   outer
//! issue: 4376
module Conf;

public class Widget
  public constructor()
  end constructor;

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
    do
      using var b := a;        // move-only local-to-local: moves; `using var` adds only the pin
      Console.WriteLine("inner");
    end;                       // `b` disposed here — proves the obligation left `a`
    Console.WriteLine("outer");
  end Main;
end Program;
