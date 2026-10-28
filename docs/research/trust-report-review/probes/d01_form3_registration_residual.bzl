module Conf;
// Family D: form-3 bounding (an `owned` return assigned to an owned field of `this`) — the registration-lifetime residual. EXPECT: report at the Subscribe call.
public class Publisher
  private owned readonly _clicked: EventSource<int> := new EventSource<int>();
  public property Clicked: IEvent<int> => _clicked;
  public method Fire(x: int)
    _clicked.Raise(x);
  end Fire;
end Publisher;
public class Listener
  private owned _sub: Subscription;
  private _count: int := 0;
  public constructor(pub: Publisher)
    _sub := pub.Clicked.Subscribe(lambda(e: int) => do _count := _count + 1; end);
  end constructor;
  public property Count: int => _count;
end Listener;
