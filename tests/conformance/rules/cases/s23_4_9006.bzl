//! case: s23_4_9006
//! doc: spec
//! section: 23.4
//! title: a generic class's virtual indexer over T? with an empty setter ignores the stored value
//! rule: Indexers may be overloaded by parameter type list. They participate in
//!   `virtual`/`override`/`sealed`/`abstract` like methods.
//! cites: 23.3, 12.5
//! expect: run
//! output: none
//! notes: Harvested from the #9152 review (q11, with the printed marker changed from a constant to the read
//!   value). Trace: the write `g[0] := "hello"` runs the empty setter, so `_t` stays null; `g[0]` is a
//!   `string?` that is null, so `?? "none"` gives "none".
module Conf;
public class G<T>
  private _t: T? := null;
  public virtual property [i: int]: T?
    get => _t;
    set => do end;
  end;
  public constructor() end constructor;
end G;
public class Program
  public static method Main(): int
    using var g := new G<string>();
    g[0] := "hello";
    Console.WriteLine(g[0] ?? "none");
    return 0;
  end Main;
end Program;
