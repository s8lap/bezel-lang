//! case: s14_2_0087
//! doc: spec
//! section: 14.2
//! title: a nullable reference type argument does not satisfy where T: class
//! rule: "`where T: class` admits non-nullable reference types of any kind" (§3.2, v0.340); "`where T: class` | Non-nullable reference types only" (§14.2 constraint table, v0.340). `D?` and `string?` are nullable, so each instantiation is rejected.
//! cites: 3.2, 14.2
//! expect: reject
//! diagnostic: does not satisfy the constraint
module Conf;

public class D
end D;

public class Program
  public static method G<T>(v: T): string where T: class
    return (case v of var x: Object => "bind", _ => "other");
  end G;

  public static method Main()
    Console.WriteLine(G<D?>(new D()));
    Console.WriteLine(G<string?>(null));
  end Main;
end Program;
