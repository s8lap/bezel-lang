module LibIt;

public abstract class Fl
  public abstract method M(q: () -> int): IEnumerable<int>;

  public virtual method V(q: () -> int): IEnumerable<int>
    yield return q();
  end V;

  public method D(q: () -> int): IEnumerable<int>
    yield return q();
    yield return q();
  end D;

  public async method A(q: () -> int): Task<int>
    await Task.Delay(1);
    return q();
  end A;
end Fl;
