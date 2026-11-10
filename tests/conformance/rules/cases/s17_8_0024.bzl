//! case: s17_8_0024
//! doc: spec
//! section: 17.8
//! title: a null-forgiving read of a borrowing arm binding over a nullable owning choice is rejected
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
  public static method Main()
    using var o: Outer? := Outer.W(new Widget(), new Widget());
    case o of
      var y => do var z := y!; end
    end case;
  end Main;
end Program;
