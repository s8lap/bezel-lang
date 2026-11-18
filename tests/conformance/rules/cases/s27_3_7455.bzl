//! case: s27_3_7455
//! doc: spec
//! section: 27.3
//! title: compound assignments over values read from assigned lambdas use the expected return type
//! rule: its inferred return type must equal the target's return slot or implicitly convert to it
//! cites: 27.3, 7.4
//! expect: run
//! output: 6
//!   30
//!   2.5
module Conf;
public class P
  public static method Main(): int
    const k: int? := 5;
    var x: int := 1;
    var f: () -> int := lambda() => 1;
    f := lambda() => k;
    x += f();
    Console.WriteLine(x);
    var fs := new Array<() -> int>();
    fs.Add(lambda() => 1);
    fs[(lambda() => 0)()] := lambda() => k;
    x *= fs[0]();
    Console.WriteLine(x);
    const d: double? := 2.5;
    var y: double := 1.0;
    var g: () -> double := lambda() => 1.0;
    g := lambda() => d;
    y *= g();
    Console.WriteLine(y);
    return 0;
  end Main;
end P;
