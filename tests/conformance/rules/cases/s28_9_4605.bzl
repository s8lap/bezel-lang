//! case: s28_9_4605
//! doc: spec
//! section: 28.9
//! title: after swapping array elements through a local, every embedded string is still readable
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 4.6, 28.11
//! expect: run
//! stdout: 4076
module Conf;

public record E(Name: string, N: int);

public class Program
  public static method Build(i: int): owned Array<E>
    var rows := new Array<E>();
    var f := $"name{i},zz{i},q{i}".Split(',');
    rows.Add(new E(f[0], 1));
    rows.Add(new E(f[1], 2));
    rows.Add(new E(f[2], 3));
    return rows;
  end Build;
  public static method Junk()
    var junk := new Array<string>();
    for var k in 1..50 do
      junk.Add($"junkjunkjunk{k}");
    end for;
  end Junk;
  public static method Main()
    var total := 0;
    for var i in 1..200 do
      var rows := Build(i);
      var t := rows[0];
      rows[0] := rows[1];
      rows[1] := t;
      Junk();
      for var r in rows do
        total := total + r.N + r.Name.Length;
      end for;
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
