//! case: s5_1_4202
//! doc: spec
//! section: 5.1
//! title: an unannotated FrozenArray<int> does not convert to IEnumerator<int>
//! rule: a type to an interface it implements or conforms to
//! cites: 5.1, stdlib 4.19
//! expect: reject
//! diagnostic: has a type that is not implicitly convertible
module Conf;

public class Program
  static method N(x: IEnumerator<int>): int => 0;

  public static method Main(): int
    var a: Array<int> := [1, 2, 3];
    using var g := FrozenArray<int>.From(a);
    Console.WriteLine(N(g));
    return 0;
  end Main;
end Program;
