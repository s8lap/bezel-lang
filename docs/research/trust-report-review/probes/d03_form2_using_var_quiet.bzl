module Conf;
// Family D control: form 2 (a `using var` local bounds the subscription) — fully static, no residual. EXPECT: quiet.
public class Publisher
  private owned readonly _clicked: EventSource<int> := new EventSource<int>();
  public property Clicked: IEvent<int> => _clicked;
  public method Fire(x: int)
    _clicked.Raise(x);
  end Fire;
end Publisher;
public class Program
  public static method Run(pub: Publisher): int
    var count := 0;
    using var sub := pub.Clicked.Subscribe(lambda(e: int) => do count := count + 1; end);
    pub.Fire(1);
    return count;
  end Run;
end Program;
