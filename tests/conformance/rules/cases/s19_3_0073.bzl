//! case: s19_3_0073
//! doc: spec
//! section: 19.3
//! title: a typed binding over an Object scrutinee matches only the box's own value type
//! rule: "A type-test operand and a pattern scrutinee are not such positions. Boxed values are
//!   runtime-managed. The wrapper's payload is fixed at boxing: a value enters a box by
//!   being copied in and leaves it by being read out — a narrowed read, an unboxing `as`, or
//!   a typed-pattern binding over a boxed scrutinee" (§8.6, v0.340). A box holds a `long` or
//!   an `int`, not both: `var n: int` does not read out a boxed `long`, and `var m: long`
//!   does not read out a boxed `int`, even though `int` converts to `long` (§5.1) — pattern
//!   position performs no conversion (§19.3).
//! cites: 8.6, 5.1
//! expect: run
//! output: long 5000000001
//!   int 5
module Conf;

public class Program
  public static method Main()
    var o: Object := 5000000001L;
    case o of
      var n: int => Console.WriteLine("int " + n.ToString()),
      var m: long => Console.WriteLine("long " + m.ToString()),
      _ => Console.WriteLine("other");
    end;
    var o2: Object := 5;
    case o2 of
      var m: long => Console.WriteLine("long " + m.ToString()),
      var n: int => Console.WriteLine("int " + n.ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
