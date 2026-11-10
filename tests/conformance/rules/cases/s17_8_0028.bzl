//! case: s17_8_0028
//! doc: spec
//! section: 17.8
//! title: under an owning match y! is classified as the bare read y and receives the value
//! rule: Over a named owned binding, `x!` is therefore classified by its position exactly as the bare read `x` is
//! cites: 15.3, 35.5
//! expect: run
//! output: 2
//! issue: 7892
module Conf;
public class Widget
  public static _n: int := 0;
  public constructor() end constructor;
  destructor Widget._n := Widget._n + 1; end destructor;
  public method Ping(): int => 7;
end Widget;
public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer;
public class Program
  static method Body()
    var o := Outer.W(new Widget(), new Widget());
    case move o of
      var y => do var z := y!; end
    end case;
  end Body;

  public static method Main()
    Body();
    Console.WriteLine(Widget._n);
  end Main;
end Program;
