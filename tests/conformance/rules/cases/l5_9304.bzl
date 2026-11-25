//! case: l5_9304
//! doc: stdlib
//! section: 5
//! title: a group whose element type differs from its key type enumerates its elements
//! rule: public interface IGrouping<TKey, T>: IEnumerable<T>
//! cites: 5
//! expect: run
//! output: aa len 2
//!   aa len 2
//!   b len 1
//!   ccc len 3
//!   2 w aa
//!   2 w aa
//!   3 w ccc
//! notes: Harvested from the #8542 review (p6). Source is ["aa","b","aa","ccc"]. First loop: key is the
//!   string itself (K = string, T = string); groups in first-occurrence order aa [aa, aa], b [b], ccc [ccc];
//!   Select over a group yields each element's Length, giving the four lines. Second loop: key is the length
//!   (K = int, T = string); groups 2 [aa, aa], 1 [b], 3 [ccc]; Where(x.Length > 1) keeps aa, aa from key 2,
//!   nothing from key 1 (b has length 1), ccc from key 3. `x.Length` on an int key would not compile, so the
//!   case needs the element to be the string.
module Conf;
public class Program
  public static method Main(): int
    using var ss := new Array<string>();
    ss.Add("aa"); ss.Add("b"); ss.Add("aa"); ss.Add("ccc");
    for var g in ss.GroupBy(lambda(s: string) => s) do
      for var u in g.Select(lambda(x) => x.Length) do
        Console.WriteLine(g.Key + " len " + u.ToString());
      end for;
    end for;
    for var g in ss.GroupBy(lambda(s: string) => s.Length) do
      for var u in g.Where(lambda(x) => x.Length > 1) do
        Console.WriteLine(g.Key.ToString() + " w " + u);
      end for;
    end for;
    return 0;
  end Main;
end Program;
