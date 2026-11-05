//! case: s28_3_9_1202
//! doc: spec
//! section: 28.3.9
//! title:moving an owned class local into an owned parameter is admitted when its alias is dead
//! rule:Consuming an owned **binding** — a local or an `owned` parameter — while a live local
//!   borrow of its current value exists is a compile error
//! cites: 28.7
//! expect: run
//! stdout: 1
module Conf;

public class Box
  public N: int := 1;

  public constructor()
  end constructor;
end Box;

public class Program
  static method SinkB(owned b: Box): int
    return b.N;
  end SinkB;

  public static method Main()
    var k := 0;
    var a := new Box();
    var b := a;
    k := k + SinkB(move a);

    Console.WriteLine(k);
  end Main;
end Program;
