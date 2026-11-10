//! case: s17_8_0026
//! doc: spec
//! section: 17.8
//! title: binding the bare read of a borrowing case-arm binding to a local is rejected (control)
//! rule: the binding is a borrow of the value: a copy is forbidden and the case construct never moves from the scrutinee, so the borrow form is the only consistent option
//! cites: 15.3, 19.3, 35.5
//! expect: reject
//! diagnostic: borrow
module Conf;
public class Widget
  public constructor() end constructor;
  public method Ping(): int => 7;
end Widget;
public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer;
public class Program
  public static method Main()
    using var o := Outer.W(new Widget(), new Widget());
    case o of
      var y => do var z := y; end
    end case;
  end Main;
end Program;
