//! case: s23_3_7901
//! doc: spec
//! section: 23.3
//! title: a setter's lambda capturing value, with the stored text read back from the lambda,
//!   runs 200 times
//! rule: The implicit setter parameter is `value`. It is a parameter of the setter
//! cites: 23.3, 27.5
//! expect: run
//! stdout: 690
//! notes: Fix pin (#8942): a setter lambda capturing `value`; the corpus runner has no leak
//!   leg, so this asserts the program output; leak freedom is covered by the gtest rows.
module M;
import System;
public class H
  public s: string := "";
  public property P: string
    get => s;
    set => do var k := lambda(): string => value; s := k(); end;
  end;
end H;

public static class Program
  public static method Main(args: Array<string>): int
    using var h := new H();
    var tot := 0;
    for var i in 0..<200 do
      h.P := "v" + i.ToString(); tot := tot + h.s.Length;
    end;
    Console.WriteLine(tot);
    return 0;
  end;
end Program;
