//! case: l5_9001
//! doc: stdlib
//! section: 5
//! title: lazy and eager method-group selectors over one source give the documented answers
//! rule: are call-bounded as their unmarked signatures state — they invoke the callback during the call and
//!   retain nothing.
//! expect: run
//! output: 6400
//! notes: Harvested from the #9012 review (p1). Lazy `Where(F)`/`Select(G)` retain their method groups in the
//!   query value; the eager `Sum(P)` retains nothing. Trace per iteration over [1,2,3]: Where(i>1) ->
//!   [2,3], Select(*2) -> [4,6]; Sum(P) = 10; the bound query again = 10; ToArray().Count = 2;
//!   enumerating a fresh query = 4 + 6 = 10; 32 per iteration x 200 = 6400. Value-only; the leak the fix
//!   removed is not visible to the corpus (no leak leg), so green on both arms.
module Conf;
public class Program
  static method F(i: int): bool => i > 1;
  static method G(i: int): int => i * 2;
  static method P(i: int): int => i;
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var a := new Array<int>();
      a.Add(1); a.Add(2); a.Add(3);
      t := t + a.Where(F).Select(G).Sum(P);
      var q := a.Where(F).Select(G);
      t := t + q.Sum(P);
      var q2 := a.Where(F).Select(G);
      using var l := q2.ToArray();
      t := t + l.Count;
      var q3 := a.Where(F).Select(G);
      for var x in q3 do t := t + x; end for;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
