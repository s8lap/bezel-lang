//! case: s21_9_9601
//! doc: spec
//! section: 21.9
//! title: a chain-call result bound with `using var` is admitted and readable
//! rule: An expression statement whose result is a non-`()` type is permitted but
//!   the compiler emits a warning: the result is silently discarded
//! cites: 21.9, 28.3
//! expect: run
//! stdout: 300
//! issue: 9245
module Conf;

public class W
  public V: int := 3;

  destructor
  end destructor;
end W;

public class T
  public V: int := 3;
end T;

public class C
  public static method Make(): () -> owned W
    return lambda(): W => new W();
  end Make;

  public static method MakeT(): () -> owned T
    return lambda(): T => new T();
  end MakeT;

  public static method MakeV(): () -> ()
    return lambda() => do end;
  end MakeV;

  public static method Main()
    var total := 0;
    for var i in 0 ..< 100 do
      using var w := C.Make()();
      var v := w.V;
      total := total + v;
    end for;
    Console.WriteLine(total);
  end Main;
end C;
