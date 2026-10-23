//! case: s22_5_0015
//! doc: spec
//! section: 22.5
//! title: the same arity check applies at a user interface's inherited member
//! rule: Each supplied argument binds **exactly one** parameter and each
//!   parameter is bound by **at most one** supplied argument: naming a
//!   parameter already bound positionally ... or naming the same parameter
//!   twice, is a compile error at the call naming that parameter.
//!   Inside the generic body, T's surface includes IFoo's members, regardless
//!   of which conformance category T belongs to (§14.2).
//! cites: 22.5, 14.2
//! expect: reject
//! diagnostic: argument
module Conf;

public interface MyI: IList<int>
end MyI;

public class Program
  public static method G<T>(x: T): int where T: MyI
    x.Add(1, 2);
    return 0;
  end G;

  public static method Main(): int
    var a: Array<int> := [1, 2, 3];
    Console.WriteLine(Program.G(a));
    return 0;
  end Main;
end Program;
