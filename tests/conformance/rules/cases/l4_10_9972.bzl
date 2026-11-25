//! case: l4_10_9972
//! doc: stdlib
//! section: 4.10
//! title: nullable-element sets stay consistent over a loop of run-time-built strings and ints
//! rule: public method TryAdd(item: T): bool; // returns whether added
//! cites: 4.10, 3.6, 8.4, 4.1
//! expect: run
//! issue: 9277
//! stdout: 600
module Conf;

public class Program
  public static method Make(i: int): string => "v" + (i - (i / 7) * 7).ToString();

  public static method Main()
    var tot := 0;
    for var i in 0 ..< 200 do
      var t := new Set<string?>();
      _ := t.TryAdd(Make(i));
      _ := t.TryAdd(Make(i));
      _ := t.TryAdd("lit");
      _ := t.TryAdd("lit");
      _ := t.TryAdd(null);
      _ := t.TryAdd(Make(i + 1));
      _ := t.TryAdd(null);
      _ := t.Remove(Make(i));
      _ := t.Remove(null);
      var j := new Set<int?>();
      _ := j.TryAdd(i);
      _ := j.TryAdd(null);
      _ := j.TryAdd(i);
      _ := j.Remove(i);
      tot := tot + t.Count + j.Count;
    end;
    Console.WriteLine(tot);
  end Main;
end Program;
