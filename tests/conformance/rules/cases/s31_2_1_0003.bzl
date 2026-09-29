//! case: s31_2_1_0003
//! doc: spec
//! section: 31.2.1
//! title: a multi-type binding types at the nearest common base
//! rule: When binding, `name` has the static type of the nearest common
//!   base of the listed types — the most specific class that is a base of
//!   every listed type.
//! cites: 31.2.1
//! expect: run
//! stdout: 2
module Conf;

public class NetworkException: Exception
  public constructor(message: string) base(message);
  end constructor;

  public virtual method Code(): int => 0;
end NetworkException;

public class TimeoutNetException: NetworkException
  public constructor(message: string) base(message);
  end constructor;

  public override method Code(): int => 1;
end TimeoutNetException;

public class ConnectException: NetworkException
  public constructor(message: string) base(message);
  end constructor;

  public override method Code(): int => 2;
end ConnectException;

public class Program
  public static method Main()
    try
      throw new ConnectException("refused");
    catch
      var ex: TimeoutNetException | ConnectException => Console.WriteLine(ex.Code()),
    end try;
  end Main;
end Program;
