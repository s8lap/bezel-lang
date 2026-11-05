//! case: l4_8_0901
//! doc: stdlib
//! section: 4.8
//! title: DictionaryEntry is constructed by its positional primary constructor, positionally or by name
//! rule: public record DictionaryEntry<K, V>(Key: K, Value: V);
//! cites: 4.8
//! expect: run
//! output: ab
//!   5
//!   zz
//!   7
//!   3
//!   4
//! issue: 8136
//! notes: Spec 9.1 gives a positional record a primary constructor over its fields in declaration order;
//!   spec 22.5 admits named arguments at a record construction.
module Conf;

public class Program
  public static method Main()
    var e := new DictionaryEntry<string, int>("ab", 5);
    Console.WriteLine(e.Key);
    Console.WriteLine(e.Value);
    var n := new DictionaryEntry<string, int>(Value := 7, Key := "zz");
    Console.WriteLine(n.Key);
    Console.WriteLine(n.Value);
    var l := new DictionaryEntry<long, int>(3, 4);
    Console.WriteLine(l.Key);
    Console.WriteLine(l.Value);
  end Main;
end Program;
