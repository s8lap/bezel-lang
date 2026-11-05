//! case: l2_1_2105
//! doc: stdlib
//! section: 2.1
//! title: a present and an absent TextWriter? field are told apart by a
//!   null test
//! rule: `Console.Write` and `Console.WriteLine` **are**
//!   `Console.Out.Write` and `Console.Out.WriteLine`: one destination, so
//!   output written through the two spellings interleaves in call order.
//! cites: 2.1, 14.1, 7.1
//! expect: run
//! output: present
//!   absent
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
end Box;

public class Program
  public static method Main()
    var b := new Box<TextWriter?>(Console.Out);
    if b.v <> null then
      b.v!.WriteLine("present");
    end;
    var c := new Box<TextWriter?>(null);
    if c.v = null then
      Console.WriteLine("absent");
    end;
    Console.WriteLine("end");
  end Main;
end Program;
