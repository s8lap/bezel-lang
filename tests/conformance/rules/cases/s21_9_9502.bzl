//! case: s21_9_9502
//! doc: spec
//! section: 21.9
//! title: a chain-call result discarded with the discard statement is admitted and disposed
//! rule: the warning is replaced by a hard error directing the caller to the discard `_ := expression;`
//! cites: 21.9, 17.13
//! expect: run
//! output: ok
//! notes: Accepting twin of s21_9_9501: the remedy the rule names, `_ := expression;`, disposes the
//!   chain-call result immediately (17.13) and compiles without the hard error.
module Conf;

public class W
  public V: int := 3;
  destructor
  end destructor;
end W;

public class C
  public static method Make(): () -> owned W
    return lambda(): W => new W();
  end Make;

  public static method Main(): int
    for var j in 0 ..< 200 do
      _ := C.Make()();
    end for;
    Console.WriteLine("ok");
    return 0;
  end Main;

end C;
