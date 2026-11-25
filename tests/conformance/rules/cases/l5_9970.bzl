//! case: l5_9970
//! doc: stdlib
//! section: 5
//! title: Single(pred) over a SortedDictionary answers the sole matching entry (captured local)
//! rule: `Single(pred)` the sole matching element
//! cites: 5, 4.17
//! expect: run
//! issue: 9290
//! stdout: 3 30
module Conf;

public class Program
  public static method Main()
    using var sd := new SortedDictionary<int, int>();
    sd.Add(3, 30);
    sd.Add(1, 10);
    sd.Add(2, 20);
    var want := 3;
    var hit := sd.Single(lambda(en: DictionaryEntry<int, int>) => en.Key = want);
    Console.WriteLine($"{hit.Key} {hit.Value}");
  end Main;
end Program;
