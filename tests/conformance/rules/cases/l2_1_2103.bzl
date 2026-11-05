//! case: l2_1_2103
//! doc: stdlib
//! section: 2.1
//! title: a non-null Box<TextWriter> field is written through, read by
//!   method, and reassigned
//! rule: `Console.Write` and `Console.WriteLine` **are**
//!   `Console.Out.Write` and `Console.Out.WriteLine`: one destination, so
//!   output written through the two spellings interleaves in call order.
//! cites: 2.1, 14.1
//! expect: run
//! output: field
//!   getter
//!   reassigned
//!   end
//! issue: 7792
//! notes: Red on base: the pre-fix compiler (764e6f9b) failed this program,
//!   with an internal #1386 lowering error or a wrong result, because a
//!   TextWriter type argument had no runtime representation in a generic
//!   instantiation. It passes on the staging tip.
module Conf;

public class Box<T>
  public v: T;

  public constructor(x: T)
    v := x;
  end constructor;

  public method Get(): T
    return v;
  end Get;
end Box;

public class Program
  public static method Main()
    var b := new Box<TextWriter>(Console.Out);
    b.v.WriteLine("field");
    b.Get().WriteLine("getter");
    var other: TextWriter := Console.Out;
    b.v := other;
    b.v.WriteLine("reassigned");
    Console.WriteLine("end");
  end Main;
end Program;
