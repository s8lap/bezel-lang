//! case: s5_1_0136
//! doc: spec
//! section: 5.1
//! title: the same argument-type check applies at a user interface's inherited member
//! rule: A value of one type implicitly converts to another when no
//!   programmer-visible information is lost — the §5.1 conversions are the
//!   only ones admitted at an argument position, so an argument whose type
//!   does not convert to the parameter's type is not applicable to the
//!   candidate (§22.6).
//!   Inside the generic body, T's surface includes IFoo's members, regardless
//!   of which conformance category T belongs to (§14.2).
//! cites: 5.1, 22.6, 14.2
//! expect: reject
//! diagnostic: implicitly convert
module Conf;

public interface MyI: IList<int>
end MyI;

public class Program
  public static method G<T>(x: T): int where T: MyI
    x.Add("not an int");
    return 0;
  end G;

  public static method Main(): int
    var a: Array<int> := [1, 2, 3];
    Console.WriteLine(Program.G(a));
    return 0;
  end Main;
end Program;
