//! case: s26_5_2309
//! doc: spec
//! section: 26.5
//! title: a predicate Count on a Dictionary is refused
//! rule: the call is a compile error citing the instance member(s); it does
//!   not fall back to an extension
//! cites: 5, 26.5
//! expect: reject
//! diagnostic: instance member
//! notes: Green on base and tip (control: the Dictionary head was already
//!   refused by #3342/#7597/#7700).
module Conf;

public class Program
  public static method Main()
    var d: Dictionary<int, int> := new Dictionary<int, int>();
    d.Add(1, 2);
    Console.WriteLine(d.Count(lambda(e: DictionaryEntry<int, int>) => e.Value > 1));
  end Main;
end Program;
