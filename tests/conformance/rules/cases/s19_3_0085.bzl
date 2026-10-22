//! case: s19_3_0085
//! doc: spec
//! section: 19.3
//! title: an Object binding over a T = D? class substitution matches a non-null instance
//! rule: "Against a reference type `R` ... a nullable substitution counting as its underlying type ... where the scrutinee's type at the instantiation is a reference type it is the ordinary runtime test" (§19.3, v0.340): a reference substitution forms no box, so only a value substitution is rejected, and a type pattern never matches null (§19.5).
//! cites: 19.5, 3.6
//! expect: run
//! stdout: bind
module Conf;

public class D
end D;

public class Program
  public static method G<T>(v: T): string
    return (case v of var x: Object => "bind", _ => "other");
  end G;

  public static method Main()
    Console.WriteLine(G<D?>(new D()));
  end Main;
end Program;
