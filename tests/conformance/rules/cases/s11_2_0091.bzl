//! case: s11_2_0091
//! doc: spec
//! section: 11.2
//! title: an alias of a built-in type in an interface parameter matches the target spelled directly
//! rule: A type alias is a pure synonym (27.8); an implementer whose parameter is the alias target spelled directly matches the interface's declared parameter (11.2: the parameter types are exactly equal under the substitution, and an alias denotes its target).
//! cites: 27.8, 14.2
//! expect: run
//! stdout: 2
module Conf;

type A := Array<int>;

public interface ISink
  method Show(m: A): int;
end ISink;

public class Sink: ISink
  public constructor()
  end constructor;

  public method Show(m: Array<int>): int => m.Count;
end Sink;

public class Program
  public static method Main()
    var s: ISink := new Sink();
    var xs: Array<int> := [1, 2];
    Console.WriteLine(s.Show(xs).ToString());
  end Main;
end Program;
