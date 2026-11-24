//! case: s28_3_11_7901
//! doc: spec
//! section: 28.3.11
//! title: an owning-choice result of a discarded call whose argument was moved from a local
//!   is disposed
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is
//!   produced, after every use of it within that expression has completed
//! cites: 28.3.11, 35.4
//! expect: run
//! stdout: 200
module M;
import System;

public class Res
  public static N: int := 0;
  public constructor() end constructor;
  destructor Res.N := Res.N + 1; end destructor;
end Res;

public choice OChoice Text(owned r: Res), Empty;
end OChoice;

public class H
  public constructor() end constructor;
  public method Pass(owned o: OChoice): OChoice return o; end Pass;
end H;

public class Program
  public static method Main()
    var h := new H();
    var i := 0;
    while i < 200 do
      var s := OChoice.Text(new Res());
      h.Pass(move s);
      i := i + 1;
    end while;
    Console.WriteLine(Res.N);
  end Main;
end Program;
