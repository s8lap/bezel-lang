public class EventSource<T>: IEvent<T>
  // No public constructor; obtained via event declaration.
  public method Subscribe(handler: (T) -> ()): owned Subscription;
  public method Raise(payload: T);
  public method Add(handler: (T) -> ());
  public method Remove(handler: (T) -> ());
end EventSource;

public class AsyncEventSource<T>: IAsyncEvent<T>
  public method Subscribe(handler: (T) -> Task): owned Subscription;
  public async method RaiseAsync(payload: T): Task;
  public method Add(handler: (T) -> Task);
  public method Remove(handler: (T) -> Task);
end AsyncEventSource;
