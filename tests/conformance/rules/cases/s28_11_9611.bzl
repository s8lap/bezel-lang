//! case: s28_11_9611
//! doc: spec
//! section: 28.11
//! title: a fresh builtin collection moved into an OwningDictionary field is admitted
//! rule: own their elements: every ingress member takes ownership, and the
//!   collection's `Dispose()` cascades through every element
//! cites: 28.11
//! expect: run
//! stdout: 100
//! issue: 9182
module Conf;

public class C
  private owned _d: OwningDictionary<string, Dictionary<string, int>> := new OwningDictionary<string, Dictionary<string, int>>();

  public method Put(k: string)
    var inner := new Dictionary<string, int>();
    inner["x"] := 1;
    _d[k] := move inner;
  end Put;

  public method Get(k: string): int
    return _d[k]["x"];
  end Get;
end C;

public class Program
  public static method Main()
    var total := 0;
    for var i in 0 ..< 100 do
      using var c := new C();
      c.Put("a");
      var v := c.Get("a");
      total := total + v;
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
