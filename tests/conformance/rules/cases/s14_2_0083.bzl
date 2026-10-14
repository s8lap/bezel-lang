//! case: s14_2_0083
//! doc: spec
//! section: 14.2
//! title: a generic body's one-argument OwningBuffer<T> runs at an enum with a zero-valued constant
//! rule: In a generic body, the one-argument constructor is read at each instantiation; at an enum that declares a zero-valued constant, default(T) is defined (17.11), so the instantiation is admitted.
//! cites: 14.2, 17.11
//! expect: run
//! stdout: 4
//! issue: 5821
module Conf;

public enum Level None := 0, High := 2; end Level;

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
