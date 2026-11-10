//! case: s17_8_0021
//! doc: spec
//! section: 17.8
//! title: returning the null-forgiving read of a borrowing case-arm binding of an owning choice is rejected
//! rule: if `x` is borrowed, `x!` is borrowed with the same source. The operator is purely a type-system assertion (with a runtime null check); it does not move, copy, or borrow beyond what reading `x` already does.
//! cites: 15.3, 19.3, 35.5
//! expect: reject
//! diagnostic: move
//! issue: 7892
module Conf;
public class Widget
  public constructor() end constructor;
  public method Ping(): int => 7;
end Widget;
public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer;
public class Program
  static method Peel(): Outer
    using var o := Outer.W(new Widget(), new Widget());
    case o of
      var y => do return y!; end
    end case;
    return Outer.X;
  end Peel;

  public static method Main()
    var r := Peel();
  end Main;
end Program;
