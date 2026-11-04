//! case: l5_0805
//! doc: stdlib
//! section: 5
//! title: Min and Max with a selector throw InvalidOperationException on an empty source, also after Where
//! rule: `Min`/`Max` — selector and no-selector forms alike — and `Average` throw `InvalidOperationException`
//! cites: 5
//! expect: run
//! stdout: True True True True
//! issue: 8108
module Conf;

public record H(N: int, Dec: decimal);

public class Program
  public static method Main()
    var e := new Array<H>();
    var t1 := false;
    var t2 := false;
    var t3 := false;
    var t4 := false;
    try
      var z := e.Min(lambda(x: H) => x.Dec);
    catch InvalidOperationException => t1 := true; end;
    try
      var z := e.Max(lambda(x: H) => x.N);
    catch InvalidOperationException => t2 := true; end;
    var a := new Array<H>();
    a.Add(new H(N := 1, Dec := 2.5m));
    try
      var z := a.Where(lambda(x: H) => x.N > 100).Max(lambda(x: H) => x.Dec);
    catch InvalidOperationException => t3 := true; end;
    try
      var z := a.Where(lambda(x: H) => x.N > 100).Min(lambda(x: H) => x.N);
    catch InvalidOperationException => t4 := true; end;
    Console.WriteLine($"{t1} {t2} {t3} {t4}");
  end Main;
end Program;
