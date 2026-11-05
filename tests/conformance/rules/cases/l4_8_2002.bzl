//! case: l4_8_2002
//! doc: stdlib
//! section: 4.8
//! title: foreach over entries, Values and Keys of a Dictionary with
//!   string? values visits every entry once
//! rule: public class Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>
//! cites: 29.1, 28.9
//! expect: run
//! stdout: 4180
//! notes: Exercises a #8207 path (a Dictionary value of type string?); the
//!   release of the +1 string is not stdout-observable, only the
//!   stored values are
module Conf;

public class Program
  public static method Main()
    var tot := 0;
    var i := 0;
    while i < 200 do
      var d := new Dictionary<int, string?>();
      d[1] := "a" + (40 + i mod 10).ToString();
      d[2] := null;
      d[3] := "bb" + i.ToString();
      for var e in d do tot := tot + (e.Value ?? "").Length; end;
      for var v in d.Values do tot := tot + (v ?? "").Length; end;
      for var k in d.Keys do tot := tot + k; end;
      i := i + 1;
    end;
    Console.WriteLine(tot);
  end Main;
end Program;
