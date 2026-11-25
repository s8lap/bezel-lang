//! case: s22_12_9801
//! doc: spec
//! section: 22.12
//! title: A block-bodied static function-typed property of a generic class is read per instantiation, 200x
//! rule: A static member of a *generic* class is accessed by supplying the type arguments on the type
//!   name in receiver position
//! cites: 22.12, 23.3
//! expect: run
//! output: 1800
//! notes: Pins #9224 (pick 08508faec): a read of a block-bodied static property must invoke a receiver-
//!   less getter. G<int>.U(1) is 4 and G<string>.U(2) is 5: 9 per iteration, 1800 over 200.
module Conf;

public class G<T>
  public static property U: (int) -> int
    get => do return lambda(x: int) => x + 3; end;
  end U;
end G;

public class Program
  public static method Main(): int
    var t := 0;
    var i := 0;
    while i < 200 do
      t := t + G<int>.U(1) + G<string>.U(2);
      i := i + 1;
    end while;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
