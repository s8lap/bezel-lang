//! case: s19_3_0074
//! doc: spec
//! section: 19.3
//! title: a record destructure and a typed binding read their payload out of a boxed record
//!   scrutinee
//! rule: "A type-test operand and a pattern scrutinee are not such positions. Boxed values are
//!   runtime-managed. The wrapper's payload is fixed at boxing: a value enters a box by
//!   being copied in and leaves it by being read out — a narrowed read, an unboxing `as`, or
//!   a typed-pattern binding over a boxed scrutinee" (§8.6, v0.340). The `Object?` scrutinee
//!   holds boxed records; the destructure `Box(Pt(var a, var b), var c: long)` and the
//!   binding `var p: Pt` read the payload out of the box.
//! cites: 9.1, 19.3
//! expect: run
//! output: 7 5000000000
//!   56
module Conf;

public record Pt(X: int, Y: int);
public record Box(P: Pt, N: long);

public class Program
  public static method Main()
    var o: Object? := new Box(new Pt(3, 4), 5000000000L);
    case o of
      Box(Pt(var a, var b), var c: long) => Console.WriteLine((a + b).ToString() + " " + c.ToString()),
      _ => Console.WriteLine("other");
    end;
    var q: Object? := new Pt(7, 8);
    case q of
      var p: Pt => Console.WriteLine((p.X * p.Y).ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
