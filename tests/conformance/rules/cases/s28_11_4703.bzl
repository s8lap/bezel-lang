//! case: s28_11_4703
//! doc: spec
//! section: 28.11
//! title: a method returning a string-bearing record element of a local array keeps the string alive
//! rule: copy out (a runtime-managed reference the copy holds — a `DictionaryEntry<string, V>`'s `Key` —
//!   keeps
//! cites: 28.9, 28.11
//! expect: run
//! stdout: 290
module Conf;

public record Row(N: string, X: int);

public class Make
  public static method MkRow(n: int): Row
    var rows := new Array<Row>();
    rows.Add(new Row($"r{n}", n));
    return rows[0];
  end MkRow;
end Make;

public class Program
  public static method Main()
    var i := 0;
    var t := 0;
    while i < 100 do
      var rs := new Array<Row>();
      rs.Add(Make.MkRow(i));
      t := t + rs[0].N.Length;
      rs.Clear();
      i := i + 1;
    end while;
    Console.WriteLine(t);
  end Main;
end Program;
