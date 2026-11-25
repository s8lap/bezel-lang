//! case: s31_1_9801
//! doc: spec
//! section: 31.1
//! title: An exception thrown in a block-bodied static getter is caught by the reader's catch arm
//! rule: a thrown exception is caught by the first arm naming its class or a base of it
//! cites: 31.1, 31.2, 23.3
//! expect: run
//! output: caught
//! notes: Pins #9224 (pick 08508faec): a read of a block-bodied static property must invoke a receiver-
//!   less getter. The getter throws; the read H.N inside the try propagates the exception to the
//!   Exception arm, so caught is printed and the WriteLine of the value never runs.
module Conf;

public class H
  public static property N: int
    get => do throw new Exception("boom"); end;
  end N;
  public static method Main(): int
    try
      Console.WriteLine(H.N);
    catch
      Exception => Console.WriteLine("caught")
    end try;
    return 0;
  end Main;
end H;
