public interface IEvent<out T>
  method Subscribe(handler: (T) -> ()): owned Subscription;
end IEvent;

public interface IAsyncEvent<out T>
  method Subscribe(handler: (T) -> Task): owned Subscription;
end IAsyncEvent;
