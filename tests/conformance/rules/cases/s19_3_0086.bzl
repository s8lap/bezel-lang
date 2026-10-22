//! case: s19_3_0086
//! doc: spec
//! section: 19.3
//! title: a bare Object pattern over a T = int? value substitution takes the answer the boxed value would give
//! rule: "a type pattern without binding ... takes, allocating nothing, the answer §12.7 gives the same `is R` test there" (§19.3, v0.340); "`v is R` is true exactly when `v` is non-null and `R` is `Object` or an interface to which the boxed value converts" (§12.7, v0.340). So `case v of Object` over `int?` matches 5 and not null.
//! cites: 12.7, 19.5
//! expect: run
//! output: obj
//!   other
module Conf;

public class Program
  public static method G<T>(v: T): string => (case v of Object => "obj", _ => "other");

  public static method Main()
    Console.WriteLine(G<int?>(5));
    Console.WriteLine(G<int?>(null));
  end Main;
end Program;
