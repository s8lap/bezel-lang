//! case: s28_9_8005
//! doc: spec
//! section: 28.9
//! title: a copied record's component stored after a call through the original sums both
//! rule: Runtime management governs the *function value itself* — the heap-allocated closure
//!   object that backs it
//! cites: 28.9, 9.1, 27.5
//! expect: run
//! stdout: 1200
//! notes: Pins #8964 (+ fix-forward): a function-typed record component read must keep its
//!   closure cell alive while a stored alias survives; the corpus runner has no
//!   sanitizer leg, so this asserts the program output (a freed cell would crash or
//!   corrupt the sum).
module Conf;
record R(f: () -> int);
public class Program
  public static method Main(): int
    var acc := 0;
    var fs := new Array<() -> int>();
    for var i in 0..<200 do
      var r := new R(lambda() => 3);
      var s := r;
      var t := s;
      acc := acc + s.f();
      fs.Add(t.f);
    end;
    for var g in fs do acc := acc + g(); end;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
