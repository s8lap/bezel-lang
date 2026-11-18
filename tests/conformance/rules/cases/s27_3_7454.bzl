//! case: s27_3_7454
//! doc: spec
//! section: 27.3
//! title: a lambda assigned to a function-typed local takes the local's return type as its expected
//!   type
//! rule: its inferred return type must equal the target's return slot or implicitly convert to it
//! cites: 27.3, 7.4, 22.6
//! expect: run
//! output: 5
//!   5
//!   5000000000
//!   5
//!   5
//! notes: The double-slot rows of the source probe are left out: the printed form of a double is not
//!   what this case decides.
module Conf;
public class P
  public static method Main(): int
    const k: int? := 5;
    const l: long? := 5000000000L;
    var f: () -> int := lambda() => 1;
    f := lambda() => k;
    Console.WriteLine(f());
    var g: () -> long := lambda() => 1L;
    g := lambda() => k;
    Console.WriteLine(g());
    g := lambda() => l;
    Console.WriteLine(g());
    var p: (int) -> long := lambda(x) => 1L;
    p := lambda(x) => k;
    Console.WriteLine(p(1));
    var s: () -> int? := lambda() => null;
    s := lambda() => k;
    Console.WriteLine(s() ?? -1);
    return 0;
  end Main;
end P;
