//! case: s14_2_0088
//! doc: spec
//! section: 14.2
//! title: a member the constraint head does not declare is not found on the type parameter
//! rule: A generic body type-checks under the weakest assumption its constraints
//!   establish (§3.6 rule 2); inside the generic body, T's surface includes
//!   IFoo's members, regardless of which conformance category T belongs to
//!   — a name the head does not declare is on no surface the body may use.
//! cites: 14.2, 3.6
//! expect: reject
//! diagnostic: no member
module Conf;

public class Program
  public static method G<T>(x: T): int where T: IList<int>
    x.Nope(1);
    return 0;
  end G;

  public static method Main(): int
    var a: Array<int> := [1, 2, 3];
    Console.WriteLine(Program.G(a));
    return 0;
  end Main;
end Program;
