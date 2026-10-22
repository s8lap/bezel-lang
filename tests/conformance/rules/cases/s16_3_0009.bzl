//! case: s16_3_0009
//! doc: spec
//! section: 16.3
//! title: a Range is accepted as an element added to an Array of IEnumerable
//! rule: "`Range<T>` is a compiler-intrinsic record that implements `IEnumerable<T>`" (§16.3, v0.340); a value is admitted where an interface it implements is expected (§5.1, v0.340), so a range is accepted at an `Array<IEnumerable<int>>.Add` argument. KNOWN DIVERGENCE (#7029): the compiler rejects the conversion at this position with a diagnostic naming #7029 (an implementation limit pending a design decision; no spec sentence rejects it).
//! cites: 5.1
//! expect: run
//! stdout: 6
//! issue: 7029
module Conf;

public class Program
  public static method Main()
    var l := new Array<IEnumerable<int>>();
    l.Add(1..3);
    var n := 0;
    for var x in l[0] do n += x; end;
    Console.WriteLine(n);
  end Main;
end Program;
