//! case: s2_3_9903
//! doc: spec
//! section: 2.3
//! title: a compound assignment through a user FrozenArray<T> indexer is admitted
//! rule: every other always-available name may be shadowed by local declarations under the normal
//!   name-resolution rules
//! cites: 2.3, 2.7, 23.4, 6.6
//! expect: run
//! stdout: 10
//! issue: 9251
//! notes: Harvested from the #9251 review (u_pluseq). The user class shadows stdlib FrozenArray<T> (2.3)
//!   and declares a settable indexer, so `f[0] += 3` reads 7, writes 10. The stdlib type's get-only
//!   indexer (stdlib 4.19) is not consulted.
module Conf;

public class FrozenArray<T>
  private _items: Array<T>;

  public constructor(items: Array<T>)
    this._items := items;
  end constructor;

  public property [i: int]: T
    get => this._items[i];
    set => do this._items[i] := value; end;
  end;
end FrozenArray;

public class Program
  public static method Main()
    var s := new Array<int>();
    s.Add(7);
    var f := new FrozenArray<int>(s);
    f[0] += 3;
    Console.WriteLine(f[0]);
  end Main;
end Program;
