//! case: s15_3_3602
//! doc: spec
//! section: 15.3
//! title: a plain binding of a move-only field read disposes nothing at its scope end
//! rule: so `x` is a **borrow** of the move-only value, exactly as the `using var` pin over the same read
//!   and a `case` binding over it are: it carries no obligation and its scope end disposes nothing
//! cites: 15.3, 28.8
//! expect: run
//! output: 200
//!   200
//! issue: 7612
//! notes: Each iteration matches one payload through the borrow `x` and disposes it once, through `h`.
//!   A compiler that made `x` owning would dispose the payload twice (a fault, or 400).
module Conf;

public class Res
  public static Count: int := 0;
  public constructor() end constructor;
  destructor Res.Count := Res.Count + 1; end destructor;
end Res;

public choice OC Text(owned r: Res), Empty; end OC;

public class H
  public owned F: OC := OC.Text(new Res());
  public constructor() end constructor;
end H;

public class Program
  public static method Main()
    var hits := 0;
    for var i in 0 ..< 200 do
      do
        using var h := new H();
        var x := h.F;
        hits := hits + (case x of OC.Text(r) => 1, OC.Empty => 0);
      end;
    end for;
    Console.WriteLine(hits);
    Console.WriteLine(Res.Count);
  end Main;
end Program;
