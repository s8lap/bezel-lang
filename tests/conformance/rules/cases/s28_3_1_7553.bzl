//! case: s28_3_1_7553
//! doc: spec
//! section: 28.3.1
//! title: a read of a local after it was moved into an owned parameter is rejected
//! rule: **moved-from** — a later read is an error until reassigned
//! cites: 28.3.1
//! expect: reject
//! diagnostic: already moved
module Conf;
class C
  public static method Sink(owned a: Array<int>): int
    return a.Count;
  end Sink;
  public method Go(): int
    var ys := new Array<int>(); ys.Add(5);
    var k := C.Sink(move ys);
    return ys.Count + k;
  end Go;
  public static method Main(): int
    var n := 0;
    for var i in 1..4 do
      using var c := new C();
      n := n + c.Go();
    end;
    Console.WriteLine(n);
    return 0;
  end Main;
end C;
