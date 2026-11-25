//! case: s28_11_9609
//! doc: spec
//! section: 28.11
//! title: a fresh builtin collection stored into an OwningDictionary slot is admitted
//! rule: takes `owned`; indexer-set disposes the previous value first
//! cites: 28.11
//! expect: run
//! stdout: 100
//! issue: 9182
module Conf;

public class C
  private owned _d: OwningDictionary<string, Dictionary<string, int>> := new OwningDictionary<string, Dictionary<string, int>>();

  public method Put(k: string)
    _d[k] := new Dictionary<string, int>();
    _d[k]["x"] := 1;
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
