//! case: s28_3_11_7904
//! doc: spec
//! section: 28.3.11
//! title: discarded choice constructions with one and two owned payloads dispose every
//!   payload
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is
//!   produced, after every use of it within that expression has completed
//! cites: 28.3.11, 35.4
//! expect: run
//! stdout: 600
module M;
import System;

public class Res
  public static N: int := 0;
  public constructor() end constructor;
  destructor Res.N := Res.N + 1; end destructor;
end Res;

public choice OChoice Text(owned r: Res), Empty;
end OChoice;

public choice Two Both(owned a: Res, owned b: Res), One(owned a: Res), Nothing;
end Two;

public class Program
  public static method Main()
    var i := 0;
    while i < 200 do
      Two.Both(new Res(), new Res());
      Two.One(new Res());
      i := i + 1;
    end while;
    Console.WriteLine(Res.N);
  end Main;
end Program;
