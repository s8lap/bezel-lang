//! case: s19_3_0014
//! doc: spec
//! section: 19.3
//! title: classes have no positional-destructuring surface
//! rule: Classes do not have a positional-destructuring surface, even when
//!   they declare a primary constructor: primary-constructor parameters
//!   are not auto-promoted to public properties, and the positional shape
//!   is not part of the class's pattern-match surface.
//! cites: 19.3, 22.10
//! expect: reject
//! diagnostic: destructuring
module Conf;

public class Point(X: int, Y: int)
  public readonly property Sum: int := X + Y;
end Point;

public class Program
  public static method Main()
    var p := new Point(1, 2);
    case p of
      Point(x, y) => Console.WriteLine($"{x},{y}");,
      _           => Console.WriteLine("other");
    end case;
    _ := p;
  end Main;
end Program;
