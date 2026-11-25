//! case: s28_11_9612
//! doc: spec
//! section: 28.11
//! title: a builtin collection stored into a collection of a narrower scope is admitted
//! rule: stored into a collection that provably outlives that scope — the
//!   collection is a field, is reachable from one, is held by a parameter that
//!   is not `owned` or reached through one (caller storage, which outlives
//!   every scope of the body), escapes the method, or is a local of strictly
//!   wider scope — is rejected at the store site
//! cites: 28.11, 28.3
//! expect: run
//! stdout: 100
//! issue: 9182
module Conf;

public class C
  public static method Put(k: string): int
    var inner := new Dictionary<string, int>();
    inner["x"] := 1;
    var n := 0;
    if k = "a" then
      var outer := new Dictionary<string, Dictionary<string, int>>();
      outer[k] := inner;
      outer.Add("b", inner);
      n := outer[k]["x"];
    end if;
    return n;
  end Put;
end C;

public class Program
  public static method Main()
    var total := 0;
    for var i in 0 ..< 100 do
      var v := C.Put("a");
      total := total + v;
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
