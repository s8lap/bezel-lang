//! case: s14_2_0082
//! doc: spec
//! section: 14.2
//! title: a generic body's one-argument OwningBuffer<T> rejects an instantiation at a non-defaultable enum
//! rule: In a generic body, a use of a two-presentation type at a type argument that is a type parameter in scope is read at each instantiation; the value presentation's one-argument constructor at a non-defaultable argument rejects the instantiation, because default(T) is a compile error there (17.11).
//! cites: 14.2, 17.11
//! expect: reject
//! diagnostic: default
//! issue: 5821
module Conf;

public enum Level Low := 1, High := 2; end Level;

public class Grid<T>
  private owned _cells: OwningBuffer<T>;
  public constructor(n: int) _cells := new OwningBuffer<T>(n); end constructor;
  public method Count(): int => _cells.Length;
end Grid;

public class Program
  public static method Main()
    using var g := new Grid<Level>(4);
    Console.WriteLine(g.Count());
  end Main;
end Program;
