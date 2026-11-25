//! case: s2_3_9904
//! doc: spec
//! section: 2.3
//! title: an indexer assignment through an alias of a user FrozenArray<int> is admitted
//! rule: every other always-available name may be shadowed by local declarations under the normal
//!   name-resolution rules
//! cites: 2.3, 2.7, 23.4
//! expect: run
//! stdout: 3
//! issue: 9251
//! notes: Harvested from the #9251 review (u_alias). The alias resolves to the user class, whose indexer
//!   has a setter: 7 becomes 3.
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

type FA := FrozenArray<int>;

public class Program
  public static method Main()
    var s := new Array<int>();
    s.Add(7);
    var f: FA := new FrozenArray<int>(s);
    f[0] := 3;
    Console.WriteLine(f[0]);
  end Main;
end Program;
