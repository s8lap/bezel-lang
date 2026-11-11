//! case: s28_11_4702
//! doc: spec
//! section: 28.11
//! title: a method returning an element copy of a borrowed array parameter keeps the entry key alive
//! rule: copy out (a runtime-managed reference the copy holds — a `DictionaryEntry<string, V>`'s `Key` —
//!   keeps
//! cites: 28.9, 28.11
//! expect: run
//! stdout: 290
module Conf;

public class Pick
  public static method First(a: Array<DictionaryEntry<string, int>>): DictionaryEntry<string, int>
    return a[0];
  end First;
end Pick;

public class Program
  public static method Main()
    var i := 0;
    var t := 0;
    while i < 100 do
      var d := new Dictionary<string, int>();
      d.Add($"a{i}", 1);
      var arr := d.ToArray();
      t := t + Pick.First(arr).Key.Length;
      i := i + 1;
    end while;
    Console.WriteLine(t);
  end Main;
end Program;
