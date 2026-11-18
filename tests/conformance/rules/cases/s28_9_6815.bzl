//! case: s28_9_6815
//! doc: spec
//! section: 28.9
//! title: a function passed as `n ?? g` to a virtual method whose override stores it stays callable
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 12.7, 7.7
//! expect: run
//! stdout: 600
//! notes: The override stores a non-null argument while the base body only inspects it: 600 = 200 iterations of g(5, 2) = 3.
module Conf;

type Fn := (int, int) -> int;

public class Base
  public constructor() end constructor;
  public virtual method Keep(os: Array<Object>, o: Object?)
    if o = null then return; end;
  end Keep;
end Base;

public class Derived: Base
  public constructor() end constructor;
  public override method Keep(os: Array<Object>, o: Object?)
    if o <> null then os.Add(o!); end;
  end Keep;
end Derived;

public class Program
  public static method Fill(fs: Array<(int, int) -> int>, os: Array<Object>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k - k;
    var n: ((int, int) -> int)? := null;
    var bs: Base := new Derived();
    bs.Keep(os, n ?? g);
  end Fill;

  public static method Main()
    var t := 0;
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      var os: Array<Object> := new Array<Object>();
      Fill(fs, os, i);
      for var o in os do var q := o as Fn; if q <> null then t := t + q!(5, 2); end; end for;
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
