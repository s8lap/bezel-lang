module L2;

public class K<A, B>
  public class N
    public method Get(): int
      return 40;
    end Get;
  end N;

  public static method Make(): owned N
    return new N();
  end Make;
end K;

public class J
  public class N
    public method Get(): int
      return 500;
    end Get;
  end N;

  public static method Make(): owned N
    return new N();
  end Make;
end J;
