//! case: l2_1_2102
//! doc: stdlib
//! section: 2.1
//! title: a Pair<TextWriter?, TextWriter> built 200 times keeps both
//!   writers
//! rule: `Console.Write` and `Console.WriteLine` **are**
//!   `Console.Out.Write` and `Console.Out.WriteLine`: one destination, so
//!   output written through the two spellings interleaves in call order.
//! cites: 2.1, 14.1
//! expect: run
//! output: 200
//!   first
//!   second
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
    var k := 0;
    for var i in 0..<200 do
      var p := new Pair<TextWriter?, TextWriter>(Console.Out, Console.Out);
      if p.a <> null then
        k := k + 1;
      end;
    end for;
    Console.WriteLine(k);
    var p2 := new Pair<TextWriter?, TextWriter>(Console.Out, Console.Out);
    if p2.a <> null then
      p2.a!.WriteLine("first");
    end;
    p2.b.WriteLine("second");
    Console.WriteLine("end");
  end Main;
end Program;
