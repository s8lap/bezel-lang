//! case: s28_14_8904
//! doc: spec
//! section: 28.14
//! title: a stored closure used as the index argument of both the setter and the getter stays callable
//! rule: the callee receives the callback for the duration of the call only
//! cites: 28.14, 28.9, 23.4
//! expect: run
//! output: 4600 9
//! notes: Harvested from the #9115/#9134 review. Setter then getter with the same stored closure as the
//!   index argument: set `_v := 5 + 9`, get `14 + 9` = 23 per iteration, 200 iterations: 4600; the
//!   stored closure is still 9 afterwards. Value-only control (green on both arms).
module Conf;
public class Gen
  private _cb: () -> int;
  public method Stored(): () -> int
    return _cb;
  end Stored;
  public constructor() _cb := lambda() => 9; end constructor;
end Gen;

public class Fl
  private _v: int := 0;
  public virtual property [q: () -> int]: int
    get => _v + q();
    set => do _v := value + q(); end;
  end;
  public constructor() end constructor;
end Fl;

public class Program
  public static method Main(): int
    var t := 0;
    using var g := new Gen();
    for var j in 0 ..< 200 do
      using var fl := new Fl();
      fl[g.Stored()] := 5;
      t := t + fl[g.Stored()];
    end for;
    var f := g.Stored();
    Console.WriteLine($"{t} {f()}");
    return 0;
  end Main;
end Program;
