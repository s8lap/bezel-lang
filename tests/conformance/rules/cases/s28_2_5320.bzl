//! case: s28_2_5320
//! doc: spec
//! section: 28.2
//! title: a read through a stored borrow of an explicitly disposed Set raises DisposedAccessException
//! rule: any instance member access on a logically-disposed instance — an instance method call (including
//!   interface and virtual dispatch), a property or indexer accessor invocation, or a field access
//!   through the reference — raises `DisposedAccessException`
//! cites: 28.2, 28.3
//! expect: run
//! output: 200200
//! flags: --checked-disposal
module Conf;

public class Holder
  private _x: Set<int>;

  public constructor(x: Set<int>)
    this._x := x;
  end constructor;

  public method N(): int
    var n := 0;
    if this._x.Contains(1) then n := n + this._x.Count; end;
    return n;
  end N;
end Holder;

public class Program
  public static method Main()
    var total := 0;
    for var i in 0..<200 do
      var x := new Set<int>();
      x.Add(1);
      using var h := new Holder(x);
      total := total + h.N();
      x.Dispose();
      try
        total := total + h.N();
      catch
        DisposedAccessException => do total := total + 1000; end,
      end try;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
