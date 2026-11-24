//! case: s27_5_7922
//! doc: spec
//! section: 27.5
//! title: a function-typed local reassigned inside its capturing lambda is read back by the
//!   enclosing body
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice
//!   versa.
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 400
//! notes: Fix pin (#8978): the corpus runner has no leak leg, so this asserts the program
//!   output; leak freedom is covered by the gtest rows.
module M;
public class C
  public static method Run(): int
    var f: () -> int := lambda() => 1;
    var h := lambda() => do f := lambda() => 2; return 2; end;
    if h() = 2 then return f(); end;
    return 0;
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run(); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
