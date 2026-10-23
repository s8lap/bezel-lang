module A;

public class G<T>
  public class N
    public method Get(): int
      return 9;
    end Get;
  end N;

  public static method Make(): owned N
    return new N();
  end Make;
end G;
