//! case: s28_11_4701
//! doc: spec
//! section: 28.11
//! title: a getter returning an element copy of a borrowing collection field keeps the entry key alive
//! rule: copy out (a runtime-managed reference the copy holds — a `DictionaryEntry<string, V>`'s `Key` —
//!   keeps
//! cites: 28.9, 28.11
//! expect: run
//! stdout: 290
module Conf;

public class Holder
  private owned _items: Array<DictionaryEntry<string, int>>;

  public constructor(n: int)
    var d := new Dictionary<string, int>();
    d.Add($"a{n}", 1);
    d.Add($"bb{n}", 2);
    this._items := new Array<DictionaryEntry<string, int>>();
    for var e in d do
      this._items.Add(e);
    end for;
  end constructor;

  public method Get(i: int): DictionaryEntry<string, int>
    return this._items[i];
  end Get;
end Holder;

public class Program
  public static method Main()
    var i := 0;
    var t := 0;
    while i < 100 do
      using var h := new Holder(i);
      t := t + h.Get(0).Key.Length;
      i := i + 1;
    end while;
    Console.WriteLine(t);
  end Main;
end Program;
