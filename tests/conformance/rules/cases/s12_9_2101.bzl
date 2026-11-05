//! case: s12_9_2101
//! doc: spec
//! section: 12.9
//! title: Box<TextWriter?> does not convert to Box<TextWriter>
//! rule: Classes, records, and concrete generic types are always invariant
//! cites: 7.1, 14.1
//! expect: reject
//! diagnostic: invariant
//! notes: Green on base and on the staging tip: the over-admission control
//!   for the #7792 unit (a nullable type argument stays distinct from the
//!   non-null one).
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
    var c: Box<TextWriter> := b;
    Console.WriteLine("end");
  end Main;
end Program;
