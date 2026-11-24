//! case: s28_3_11_7902
//! doc: spec
//! section: 28.3.11
//! title: a nullable owning-choice result of a discarded call is disposed when non-null and
//!   skipped when null
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is
//!   produced, after every use of it within that expression has completed
//! cites: 28.3.11, 35.4
//! expect: run
//! stdout: 133
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
  public method MaybeMake(k: int): OChoice?
    if k mod 3 = 0 then return null; end if;
    return OChoice.Text(new Res());
  end MaybeMake;
end H;

public class Program
  public static method Main()
    var h := new H();
    var i := 0;
    while i < 200 do
      h.MaybeMake(i);
      i := i + 1;
    end while;
    Console.WriteLine(Res.N);
  end Main;
end Program;
