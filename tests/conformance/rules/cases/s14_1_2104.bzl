//! case: s14_1_2104
//! doc: spec
//! section: 14.1
//! title: static factories return an owned Box<TextWriter?> whose payload
//!   survives
//! rule: An **instantiation** of a generic declaration is the constructed
//!   type or constructed member obtained by substituting type arguments for
//!   its type parameters
//! cites: 14.1, 28.3
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

  public static method Of(x: T): owned Box<T>
    return new Box<T>(x);
  end Of;
end Box;

public class Q
  public static method Mk<T>(x: T): owned Box<T>
    return new Box<T>(x);
  end Mk;
end Q;

public class Program
  public static method Main()
    var k := 0;
    for var i in 0..<200 do
      using var a := Box<TextWriter?>.Of(Console.Error);
      if a.v <> null then
        k := k + 1;
      end;
      using var m := Q.Mk<TextWriter?>(Console.Error);
      if m.v <> null then
        k := k + 1;
      end;
    end for;
    Console.WriteLine(k);
    Console.WriteLine("end");
  end Main;
end Program;
