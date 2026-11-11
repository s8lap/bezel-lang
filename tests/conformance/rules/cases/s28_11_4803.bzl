//! case: s28_11_4803
//! doc: spec
//! section: 28.11
//! title: an int element indexed out of a local array and returned is a plain copy, summing to 100
//! rule: copy out (a runtime-managed reference the copy holds — a `DictionaryEntry<string, V>`'s `Key` — keeps [§28.9](#sec-runtime-managed-types)'s unbounded lifetime through every later mutation or disposal of the collection)
//! cites: 28.9
//! expect: run
//! output: 100
module Conf;

public class P
  public static method Get(n: int): int
    var a := new Array<int>();
    a.Add(n);
    a.Add(2);
    return a[0];
  end Get;
end P;

public class Program
  public static method Main()
    var i := 0;
    var t := 0;
    while i < 100 do
      t := t + P.Get(1);
      i := i + 1;
    end while;
    Console.WriteLine(t);
  end Main;
end Program;
