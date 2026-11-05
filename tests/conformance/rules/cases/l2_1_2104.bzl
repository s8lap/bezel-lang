//! case: l2_1_2104
//! doc: stdlib
//! section: 2.1
//! title: Pair<int, TextWriter?> and Pair<TextWriter?, string> keep the
//!   neighbouring fields intact
//! rule: `Console.Write` and `Console.WriteLine` **are**
//!   `Console.Out.Write` and `Console.Out.WriteLine`: one destination, so
//!   output written through the two spellings interleaves in call order.
//! cites: 2.1, 14.1
//! expect: run
//! output: pb
//!   3
//!   qa
//!   s
//!   end
//! issue: 7792
//! notes: Red on base: the pre-fix compiler (764e6f9b) failed this program,
//!   with an internal #1386 lowering error or a wrong result, because a
//!   TextWriter type argument had no runtime representation in a generic
//!   instantiation. It passes on the staging tip.
module Conf;

public class Pair<A, B>
  public a: A;
  public b: B;

  public constructor(x: A, y: B)
    a := x;
    b := y;
  end constructor;
end Pair;

public class Program
  public static method Main()
    var p := new Pair<int, TextWriter?>(3, Console.Out);
    if p.b <> null then
      p.b!.WriteLine("pb");
    end;
    Console.WriteLine(p.a);
    var q := new Pair<TextWriter?, string>(Console.Out, "s");
    if q.a <> null then
      q.a!.WriteLine("qa");
    end;
    Console.WriteLine(q.b);
    Console.WriteLine("end");
  end Main;
end Program;
