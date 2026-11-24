//! case: s28_9_8002
//! doc: spec
//! section: 28.9
//! title: a record component stored through a dictionary index set 200 times sums the
//!   closure results
//! rule: Runtime management governs the *function value itself* — the heap-allocated closure
//!   object that backs it
//! cites: 28.9, 9.1, 27.5
//! expect: run
//! stdout: 600
//! notes: Pins #8964 (+ fix-forward): a function-typed record component read must keep its
//!   closure cell alive while a stored alias survives; the corpus runner has no
//!   sanitizer leg, so this asserts the program output (a freed cell would crash or
//!   corrupt the sum).
module Conf;
record R(f: () -> int);
public class Program
  public static method Main(): int
    var d := new Dictionary<int, () -> int>();
    for var i in 0..<200 do
      var r := new R(lambda() => 3);
      d[i] := r.f;
    end;
    var acc := 0;
    for var i in 0..<200 do acc := acc + d[i](); end;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
