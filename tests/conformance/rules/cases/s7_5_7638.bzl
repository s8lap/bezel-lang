//! case: s7_5_7638
//! doc: spec
//! section: 7.5
//! title: a move argument of a `?.` call on an owning choice is disposed once
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access
//! cites: 7.5, 28.3, 35.9
//! expect: run
//! stdout: 200
module Conf;
import System;
class Res
  public static N: int := 0;
  public constructor() end constructor;
  destructor Res.N := Res.N + 1; end destructor;
end Res;
choice OChoice Text(owned r: Res), Empty;
end OChoice;
class H
  public constructor() end constructor;
  public method Make(): OChoice return OChoice.Text(new Res()); end Make;
  public method Drop(owned o: OChoice): OChoice return OChoice.Empty; end Drop;
end H;
public class Program
  public static method Main()
    var i := 0;
    while i < 200 do
      var h: H? := (if i mod 3 = 0 then null else new H());
      var s := OChoice.Text(new Res());
      var z := h?.Drop(move s);
      i := i + 1;
    end while;
    Console.WriteLine(Res.N);
  end Main;
end Program;
