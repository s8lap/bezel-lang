module XLC;

public class IB : Exception
  public constructor(m: string) base(m);
    Console.WriteLine("IMPURE IB(m)");
  end constructor;

  public pure constructor(m: string, k: int) base(m);
  end constructor;
end IB;
