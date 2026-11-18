//! case: s27_5_7422
//! doc: spec
//! section: 27.5
//! title: a returned closure keeps a captured parameter alive past its frame
//! rule: Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter
//!   excepted (below).
//! cites: 27.5
//! expect: run
//! output: 40000
module Conf; public class C
  public static method Make(p: int): () -> int
    var f2 := lambda() => lambda() => p + 1;
    var a := f2()();
    return lambda() => p + a;
  end Make;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do var g := C.Make(j); total := total + g(); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
