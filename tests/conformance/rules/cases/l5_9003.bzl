//! case: l5_9003
//! doc: stdlib
//! section: 5
//! title: a bound-instance method group in an eager terminal leaves the receiver's lifetime alone
//! rule: are call-bounded as their unmarked signatures state — they invoke the callback during the call and
//!   retain nothing.
//! expect: run
//! output: 13
//!   13
//!   end
//!   disp
//! notes: Harvested from the #9012 review (p4, rewritten with `using var` and a second Sum in place of the
//!   review's Any, so the program has no must-use warning and no int-returning predicate). The eager
//!   `Sum(s.P)` retains nothing, so `s` is disposed once, at the end of Main (spec 28.2/28.9). Trace:
//!   P(5) + P(6) = 6 + 7 = 13, twice; then "end"; then the destructor prints "disp" at scope end.
module Conf;
public class Sel
  public method P(i: int): int => i + 1;
  destructor
    Console.WriteLine("disp");
  end destructor;
end Sel;
public class Program
  public static method Main(): int
    var a := new Array<int>();
    a.Add(5); a.Add(6);
    using var s := new Sel();
    Console.WriteLine(a.Sum(s.P));
    Console.WriteLine(a.Sum(s.P));
    Console.WriteLine("end");
    return 0;
  end Main;
end Program;
