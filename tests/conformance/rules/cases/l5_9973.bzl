//! case: l5_9973
//! doc: stdlib
//! section: 5
//! title: Single(pred) over a SortedDictionary with string keys and values answers the matching entry
//! rule: `Single(pred)` the sole matching element
//! cites: 5, 4.17
//! expect: run
//! issue: 9290
//! stdout: mm=3
module Conf;

public class Program
  public static method Main()
    using var sk := new SortedDictionary<string, string>();
    sk.Add("zz", "1");
    sk.Add("aa", "2");
    sk.Add("mm", "3");
    var hit := sk.Single(lambda(en: DictionaryEntry<string, string>) => en.Key = "mm");
    Console.WriteLine($"{hit.Key}={hit.Value}");
  end Main;
end Program;
