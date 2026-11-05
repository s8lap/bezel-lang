//! case: s27_8_1403
//! doc: spec
//! section: 27.8
//! title: a generic alias instantiated at a method's type parameter Guid keeps its body's own Guid
//! rule: Each instantiation of the alias is interchangeable with the corresponding instantiation of
//!   the aliased type.
//! cites: 27.8, 2.7
//! expect: run
//! stdout: 1
module Conf;
type DG<T> := Dictionary<T, Guid>;
public class U
  public static method F<Guid>(d: DG<Guid>): int
    return d.Count;
  end F;
end U;
public class Program
  public static method Main()
    var d: Dictionary<int, Guid> := new Dictionary<int, Guid>();
    d[1] := Guid.NewGuid();
    Console.WriteLine(U.F<int>(d));
  end Main;
end Program;
