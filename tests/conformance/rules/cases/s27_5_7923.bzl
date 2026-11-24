//! case: s27_5_7923
//! doc: spec
//! section: 27.5
//! title: a function-typed local assigned outside and inside its capturing lambda keeps the
//!   latest store
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 1200
//! notes: Fix pin (#8978): the corpus runner has no leak leg, so this asserts the program
//!   output; leak freedom is covered by the gtest rows.
module M;
public class C
  public static method Run(): int
    var f: () -> int := lambda() => 1;
    var h := lambda() => do f := lambda() => 2; return 2; end;
    f := lambda() => 9;
    var r := h();
    f := lambda() => 4;
    return r + f();
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run(); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
