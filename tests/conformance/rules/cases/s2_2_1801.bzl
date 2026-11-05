//! case: s2_2_1801
//! doc: spec
//! section: 2.2
//! title: a user class named FrozenArray shadows the standard-library one and its own ToArray is called
//! rule: A declaration in the *current module* shadows any same-named import for
//!   bare-name purposes
//! cites: 26.5, 2.3
//! expect: run
//! output: 41
module Conf;

class FrozenArray<T>
  public method ToArray(): int => 41;
end FrozenArray;
extension IEnumerable<int>
  public method Foo(): int => 7;
end;
class Program
  public static method Main()
    using var g: FrozenArray<int> := new FrozenArray<int>();
    Console.WriteLine(g.ToArray());
  end Main;
end Program;
