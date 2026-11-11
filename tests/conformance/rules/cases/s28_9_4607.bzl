//! case: s28_9_4607
//! doc: spec
//! section: 28.9
//! title: a local-to-local copy of an array element with a live string source reads back intact
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 4.6, 28.11
//! expect: run
//! stdout: 4076
module Conf;

public record E(Name: string, N: int);

public class Program
  public static method Build(f: Array<string>): owned Array<E>
    var rows := new Array<E>();
    rows.Add(new E(f[0], 1));
    rows.Add(new E(f[1], 2));
    rows.Add(new E(f[2], 3));
    return rows;
  end Build;
  public static method Main()
    var total := 0;
    for var i in 1..200 do
      var names := $"name{i},zz{i},q{i}".Split(',');
      var rows := Build(names);
      var t := rows[0];
      var u := t;
      t := rows[1];
      rows[0] := rows[2];
      rows[2] := u;
      for var r in rows do
        total := total + r.N + r.Name.Length;
      end for;
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
