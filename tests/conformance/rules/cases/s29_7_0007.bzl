//! case: s29_7_0007
//! doc: spec
//! section: 29.7
//! title: early disposal runs the scope-end drop of an undischarged plain-var owned local
//! rule: Runs the pending cleanup actions active at the point of suspension —
//!   `using var` disposals ..., the implicit scope-end drop of an undischarged
//!   plain-`var` owned local (§28.3) and `finally` clauses — **in reverse
//!   order of establishment, exactly as if the body had exited at the
//!   suspension point**.
//! cites: 29.7, 28.3
//! expect: run
//! output: 1
//!   closed
//!   after
//! issue: 5877
module Conf;

public class Res
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Res;

public class Demo
  public static method Gen(): owned IEnumerable<int>
    var r := new Res();
    yield return 1;
    yield return 2;
  end Gen;

  public static method Main()
    for var x in Gen() do
      Console.WriteLine(x);
      break;
    end;
    Console.WriteLine("after");
  end Main;
end Demo;
