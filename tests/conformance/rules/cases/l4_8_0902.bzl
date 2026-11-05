//! case: l4_8_0902
//! doc: stdlib
//! section: 4.8
//! title: DictionaryEntry built from fresh string producers in a loop keeps the stored key and value
//! rule: public record DictionaryEntry<K, V>(Key: K, Value: V);
//! cites: 4.8
//! expect: run
//! output: 490
//! issue: 8136
//! notes: Value read-back over 100 fresh producers; the sum is derived by hand from the key lengths.
module Conf;

public class Program
  public static method Main()
    var i := 0;
    var sum := 0;
    while i < 100 do
      var k := "a" + i.ToString();
      var e := new DictionaryEntry<string, string>(k, "c" + "d");
      sum := sum + e.Key.Length + e.Value.Length;
      i := i + 1;
    end while;
    Console.WriteLine(sum);
  end Main;
end Program;
