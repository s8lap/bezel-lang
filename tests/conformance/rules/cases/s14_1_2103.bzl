//! case: s14_1_2103
//! doc: spec
//! section: 14.1
//! title: nested Box<Box<TextWriter?>> and Box<G<TextWriter?>> read through
//!   a local
//! rule: An **instantiation** of a generic declaration is the constructed
//!   type or constructed member obtained by substituting type arguments for
//!   its type parameters
//! cites: 14.1
//! expect: run
//! output: 400
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

public record G<T>(V: T);

public class Program
  public static method Main()
    var k := 0;
    for var i in 0..<200 do
      var bb := new Box<Box<TextWriter?>>(new Box<TextWriter?>(Console.Error));
      var inner := bb.v;
      if inner.v <> null then
        k := k + 1;
      end;
      var bg := new Box<G<TextWriter?>>(new G<TextWriter?>(Console.Error));
      var ig := bg.v;
      if ig.V <> null then
        k := k + 1;
      end;
    end for;
    Console.WriteLine(k);
    Console.WriteLine("end");
  end Main;
end Program;
