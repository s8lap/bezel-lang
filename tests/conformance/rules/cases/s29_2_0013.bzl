//! case: s29_2_0013
//! doc: spec
//! section: 29.2
//! title: iterating an OwningDictionary's Keys view is the borrowing remedy
//! rule: Iterate `.Keys` and `PopValue` to take, or use `Drain()`.
//! cites: 29.2
//! expect: run
//! stdout: 1
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var od := new OwningDictionary<string, Widget>();
    od["a"] := new Widget();
    var count := 0;
    for var key in od.Keys do
      count := count + 1;
    end;
    Console.WriteLine(count);
  end Main;
end Program;
