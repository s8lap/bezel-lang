//! case: s14_1_9307
//! doc: spec
//! section: 14.1
//! title: a drained entry moved into a using local reads its record key and owned value at K = Pt
//! rule: **Instantiation and its site.** An **instantiation** of a generic declaration is the constructed type or constructed member obtained by substituting type arguments for its type parameters
//! cites: 14.1, 4.9, 28.9
//! expect: run
//! output: Pt(X: 199, Y: 0):x;
//! notes: Harvested from the #9050 review (q_Pt_B_using_move_gen), reduced to one entry. Same program as s14_1_9306 with the record `Pt` for the key: its rendering is the positional record's, `Pt(X: 199, Y: 0)` (spec 8.5), so the last iteration's line is `Pt(X: 199, Y: 0):x;`.
module Conf;
public record Pt(X: int, Y: int);
public class Conn
  public Name: string;
  public constructor(n: string)
    Name := n;
  end constructor;
  destructor
  end destructor;
end Conn;
public class Repo<K> where K: hashable
  private owned _d: OwningDictionary<K, Conn> := new OwningDictionary<K, Conn>();
  public method Put(k: K, owned c: Conn) => _d[k] := c;
  public method Go(): string
    var s := "";
    for var e in _d.Drain() do
      using var x := move e;
      s := s + $"{x.Key}:{x.Value.Name};";
    end for;
    return s;
  end Go;
end Repo;
public class Program
  public static method Main(): int
    var last := "";
    var i := 0;
    while i < 200 do
      using var r := new Repo<Pt>();
      r.Put(new Pt(i, 0), new Conn("x"));
      last := r.Go();
      i := i + 1;
    end while;
    Console.WriteLine(last);
    return 0;
  end Main;
end Program;
