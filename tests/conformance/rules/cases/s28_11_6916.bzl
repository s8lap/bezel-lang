//! case: s28_11_6916
//! doc: spec
//! section: 28.11
//! title: a string copied out of a dictionary by indexer-get into a record survives Remove
//! rule: copy out (a runtime-managed reference the copy holds — a `DictionaryEntry<string,
//!   V>`'s `Key` — keeps §28.9's unbounded lifetime through every later mutation or
//!   disposal of the collection)
//! cites: 28.11, 9.7, 28.9
//! expect: run
//! stdout: 1092
module Conf;
record E(Name: string, N: int);
public class Program
  public static method Main()
    var total := 0;
    for var i in 1..200 do
      var rows := new Array<E>();
      using var dd := new Dictionary<int, string>();
      dd.Add(1, $"ds{i}");
      rows.Add(new E(dd[1], 1));
      dd.Remove(1);
      var junk := $"JUNK{i},yy{i}".Split(',');
      for var r in rows do total := total + r.Name.Length + r.N; end;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
