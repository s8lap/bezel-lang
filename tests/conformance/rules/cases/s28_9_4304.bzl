//! case: s28_9_4304
//! doc: spec
//! section: 28.9
//! title: class instances of different sizes stored in an Object field beside boxes keep their state
//! rule: so a boxed value is an immutable shared reference.
//! cites: 28.9, 8.6
//! expect: run
//! stdout: 60
module Conf;

public class Empty
  public constructor() end constructor;
end Empty;

public class Cell
  public a: int;
  public b: int;
  public constructor(a: int)
    this.a := a;
    this.b := a;
  end constructor;
end Cell;

public class Holder
  public o: Object?;
  public constructor() end constructor;
  public method Take(n: int)
    o := n;
  end Take;
  public method Put(x: Object)
    o := x;
  end Put;
  public method Read(): int
    return (o as int) ?? -1;
  end Read;
end Holder;

public class Program
  public static method Main()
    var sum := 0;
    for var i in 0..<10 do
      using var e := new Empty();
      using var c := new Cell(3);
      using var h := new Holder();
      h.Take(i);
      h.Put(e);
      h.Take(i);
      h.Put(c);
      sum += c.a + c.b;
      using var h2 := new Holder();
      h2.Put(e);
    end for;
    Console.WriteLine(sum);
  end Main;
end Program;
