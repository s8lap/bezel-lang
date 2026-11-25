//! case: s27_8_9861
//! doc: spec
//! section: 27.8
//! title: The method-reference form of ReferenceEquals through a class alias answers identity
//! rule: see through to the aliased type for identity, equality, method dispatch, and overload resolution
//! cites: 27.8, 27.6, 11.7, 8.1
//! expect: run
//! stdout: True
//! issue: 9021
//! notes: Harvested from the #9021 review (l04_KA). The read form `KA.ReferenceEquals` (a method
//!   reference, 27.6) names the same static as `K.ReferenceEquals` (27.8 sees through the alias,
//!   11.7); calling it on one instance twice answers True (8.1). The pre-fix compiler formed a
//!   degraded reference here and the program crashed.
module Conf;

public class K end K;
type KA := K;

public class Program
  public static method Main(): int
    var a := new K();
    var f := KA.ReferenceEquals;
    Console.WriteLine(f(a, a));
    return 0;
  end Main;
end Program;
