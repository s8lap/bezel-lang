//! case: s28_14_7352
//! doc: spec
//! section: 28.14
//! title: a derived constructor forwards a plain function parameter to a base
//!   plain function parameter
//! rule: passed on as an argument to another call-bounded parameter
//! cites: 28.3
//! expect: run
//! stdout: 690
//! notes: Pins that the #8778 fix does not reject call-bounded to
//!   call-bounded forwarding: the plain parameter `f` of `D` goes to the
//!   plain parameter of `P`, and nothing is stored. The lambda captures the
//!   local `s` (the string "x" followed by i's digits, length 2 for i<10, 3
//!   for i<100, 4 below 200: 690 in total).
module Conf;

public class P
  public r: int;
  public constructor(f: () -> int) r := f(); end;
end;
public class D : P
  public constructor(f: () -> int) base(f); end;
end;

public static class Program
  public static method Main(): int
    var i := 0; var tot := 0;
    while i < 200 do
      var s := "x" + i.ToString();
      using var k := new D(lambda(): int => s.Length);
      tot := tot + k.r;
      i := i + 1;
    end;
    Console.Write(tot);
    return 0;
  end;
end;
