//! case: s28_11_4802
//! doc: spec
//! section: 28.11
//! title: a DictionaryEntry returned by a computed index keeps its Key after the local array is disposed
//! rule: copy out (a runtime-managed reference the copy holds — a `DictionaryEntry<string, V>`'s `Key` — keeps [§28.9](#sec-runtime-managed-types)'s unbounded lifetime through every later mutation or disposal of the collection)
//! cites: 28.9
//! expect: run
//! output: 290
module Conf;

public class P
  public static method MakeDict(n: int): owned Dictionary<string, int>
    var d := new Dictionary<string, int>();
    d.Add($"a{n}", 1);
    d.Add($"b{n}", 2);
    return d;
  end MakeDict;

  public static method MakeEntry(n: int): owned DictionaryEntry<string, int>
    var arr := MakeDict(n).ToArray();
    return arr[arr.Count - 2];
  end MakeEntry;
end P;

public class Program
  public static method Main()
    var i := 0;
    var t := 0;
    while i < 100 do
      t := t + P.MakeEntry(i).Key.Length;
      i := i + 1;
    end while;
    Console.WriteLine(t);
  end Main;
end Program;
