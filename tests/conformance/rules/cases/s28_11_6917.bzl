//! case: s28_11_6917
//! doc: spec
//! section: 28.11
//! title: a record rebuilt from the component of a copied-out record parameter survives
//!   Remove
//! rule: copy out (a runtime-managed reference the copy holds — a `DictionaryEntry<string,
//!   V>`'s `Key` — keeps §28.9's unbounded lifetime through every later mutation or
//!   disposal of the collection)
//! cites: 28.11, 9.7, 28.9
//! expect: run
//! stdout: 1292
module Conf;
record E(Name: string, N: int);
public class Program
  static method MakeDict(i: int): owned Dictionary<int, E>
    var d := new Dictionary<int, E>();
    d.Add(1, new E($"dv{i}", 1));
    return d;
  end MakeDict;
  static method AddFromRec(rows: Array<E>, e: E)
    rows.Add(new E(e.Name, 2));
  end AddFromRec;
  public static method Main()
    var total := 0;
    for var i in 1..200 do
      var rows := new Array<E>();
      using var d := Program.MakeDict(i);
      Program.AddFromRec(rows, d[1]);
      d.Remove(1);
      var junk := $"JUNK{i},yy{i}".Split(',');
      for var r in rows do total := total + r.Name.Length + r.N; end;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
