//! case: s19_3_0083
//! doc: spec
//! section: 19.3
//! title: an Object binding over T = string? is the ordinary test and does not match null
//! rule: "Against a reference type `R` ... a nullable substitution counting as its underlying type ... where the scrutinee's type at the instantiation is a reference type it is the ordinary runtime test" (§19.3, v0.340): a reference substitution forms no box, so only a value substitution is rejected, and a type pattern never matches null (§19.5).
//! cites: 19.5, 3.6
//! expect: run
//! output: bind
//!   other
module Conf;

public class Program
  public static method G<T>(v: T): string
    return (case v of var x: Object => "bind", _ => "other");
  end G;

  public static method Main()
    Console.WriteLine(G<string?>("s"));
    Console.WriteLine(G<string?>(null));
  end Main;
end Program;
