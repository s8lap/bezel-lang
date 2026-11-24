//! case: s23_3_8001
//! doc: spec
//! section: 23.3
//! title: a block-form get accessor with an expression body in a guarded cycle reads its
//!   else lambda
//! rule: a **getter's** expression body under the property's declared type
//!   ([§23.3](#sec-block-form-properties)) is the same position
//! cites: 23.2, 23.3, 20
//! expect: run
//! stdout: 800
//! notes: Pins the #8981 fix-forward for the accessor `get =>` spelling of the cycle.
module Conf;
public class H
  private _d: int;
  public constructor() _d := 3; end;
  public property A: (int) -> int
    get => (if _d > 5 then B else lambda(x: int) => x + 1);
  end;
  public property B: (int) -> int
    get => (if _d > 6 then A else lambda(x: int) => x + 2);
  end;
end H;
public static class Program
  public static method Main(): int
    using var h := new H();
    var tot := 0;
    for var i in 0..<200 do
      var g := h.A;
      tot := tot + g(1);
      tot := tot + h.A(1);
    end;
    Console.WriteLine(tot);
    return 0;
  end;
end Program;
