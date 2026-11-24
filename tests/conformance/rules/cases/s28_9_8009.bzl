//! case: s28_9_8009
//! doc: spec
//! section: 28.9
//! title: a record component called through two reads in a loop sums the closure results
//! rule: Runtime management governs the *function value itself* — the heap-allocated closure
//!   object that backs it
//! cites: 28.9, 9.1, 27.5
//! expect: run
//! stdout: 1200
//! notes: Pins #8964's fix-forward control: a call-only read still works (the fix must not
//!   break the common call shape).
module Conf;
record R(f: () -> int);
public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0..<200 do
      var r := new R(lambda() => 3);
      acc := acc + r.f() + r.f();
    end;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
