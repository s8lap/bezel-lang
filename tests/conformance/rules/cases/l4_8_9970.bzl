//! case: l4_8_9970
//! doc: stdlib
//! section: 4.8
//! title: Single() and Single(pred) over a Dictionary answer the entry
//! rule: public class Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>
//! cites: 4.8, 5
//! expect: run
//! issue: 9290
//! stdout: 4 40
module Conf;

public class Program
  public static method Main()
    using var d := new Dictionary<int, int>();
    d.Add(4, 40);
    var hit := d.Single(lambda(en: DictionaryEntry<int, int>) => en.Key = 4);
    Console.WriteLine($"{d.Single().Key} {hit.Value}");
  end Main;
end Program;
