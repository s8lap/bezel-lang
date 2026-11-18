//! case: s7_5_7639
//! doc: spec
//! section: 7.5
//! title: `??` after `?.` on an owning choice supplies a fresh owned value when the receiver is null
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access
//! cites: 7.5, 35.9
//! expect: run
//! output: 200
//!   200
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
    var texts := 0;
    while i < 200 do
      var h: H? := (if i mod 3 = 0 then null else new H());
      var d := h?.Make() ?? OChoice.Text(new Res());
      case d of
        OChoice.Text(_) => texts := texts + 1,
        OChoice.Empty => ()
      end case;
      i := i + 1;
    end while;
    Console.WriteLine(texts);
    Console.WriteLine(Res.N);
  end Main;
end Program;
