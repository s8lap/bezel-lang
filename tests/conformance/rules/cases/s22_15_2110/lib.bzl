module XLC;

public class ImpOnly : Exception
  public constructor(m: string) base(m);
    Console.WriteLine("IMPURE ImpOnly(m)");
  end constructor;
end ImpOnly;

public class MidQ(m: string): ImpOnly(m)
  public constructor() this("q");
  end constructor;
end MidQ;
