//! case: s2_3_9905
//! doc: spec
//! section: 2.3
//! title: an indexer assignment through a user FrozenArray<int> parameter is admitted
//! rule: every other always-available name may be shadowed by local declarations under the normal
//!   name-resolution rules
//! cites: 2.3, 2.7, 23.4
//! expect: run
//! stdout: 9
//! issue: 9251
//! notes: Harvested from the #9251 review (u_param). The parameter's type is the user class, so the write
//!   through it reaches the user setter: 9.
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
  public static method Poke(f: FrozenArray<int>)
    f[0] := 9;
  end Poke;

  public static method Main()
    var s := new Array<int>();
    s.Add(7);
    var f := new FrozenArray<int>(s);
    Poke(f);
    Console.WriteLine(f[0]);
  end Main;
end Program;
