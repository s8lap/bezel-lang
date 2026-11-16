//! case: s14_2_4918
//! doc: spec
//! section: 14.2
//! title: an override omitting where inherits the INumber bound, so its body adds and the int call runs
//! rule: Omitting the `where` clauses is the recommended spelling and means exactly the inherited set
//! cites: 14.1, 11.2
//! expect: run
//! output: 14
//! issue: 8618
module Conf;
class B
  public virtual method Z<T>(a: T): T where T: INumber<T> => a + a;
end B;
class D: B
  public override method Z<T>(a: T): T => a + a;
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z(7));
    return 0;
  end Main;
end P;
