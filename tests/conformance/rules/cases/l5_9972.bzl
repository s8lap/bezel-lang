//! case: l5_9972
//! doc: stdlib
//! section: 5
//! title: Single and Single(pred) over a SortedDictionary raise unless exactly one entry matches
//! rule: `First`/`Last`/`Single` raise `InvalidOperationException`
//! cites: 5, 4.17
//! expect: run
//! issue: 9290
//! stdout: True True True True
module Conf;

public class Program
  public static method Main()
    using var empty := new SortedDictionary<int, int>();
    using var several := new SortedDictionary<int, int>();
    several.Add(1, 10);
    several.Add(2, 20);
    var t1 := false;
    var t2 := false;
    var t3 := false;
    var t4 := false;
    try
      var x := empty.Single();
    catch InvalidOperationException => t1 := true; end;
    try
      var x := several.Single();
    catch InvalidOperationException => t2 := true; end;
    try
      var x := several.Single(lambda(en: DictionaryEntry<int, int>) => en.Key > 5);
    catch InvalidOperationException => t3 := true; end;
    try
      var x := several.Single(lambda(en: DictionaryEntry<int, int>) => en.Value > 5);
    catch InvalidOperationException => t4 := true; end;
    Console.WriteLine($"{t1} {t2} {t3} {t4}");
  end Main;
end Program;
