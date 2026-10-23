module L1;

public class G<T>
  public class N
    public method Get(): int
      return 3;
    end Get;
  end N;

  public static method Make(): owned N
    return new N();
  end Make;
end G;
