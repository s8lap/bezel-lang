public interface IEvent<out T>
  method Subscribe(scoped handler: (T) -> ()): owned Subscription;
end IEvent;

public interface IAsyncEvent<out T>
  method Subscribe(scoped handler: (T) -> Task): owned Subscription;
end IAsyncEvent;

public class EventSource<T>: IEvent<T>
  public constructor();
  public method Subscribe(scoped handler: (T) -> ()): owned Subscription;
  public method Raise(payload: T);

  public method Add(scoped handler: (T) -> ());         // scoped (v0.180): Add stores — spec §28.14
  public method Remove(handler: (T) -> ());              // call-bounded: Remove only compares
  destructor;    // severs the subscriber list before reclamation (spec §28.14 quiescence)
end EventSource;

public class AsyncEventSource<T>: IAsyncEvent<T>
  public constructor();
  public method Subscribe(scoped handler: (T) -> Task): owned Subscription;
  public method RaiseAsync(payload: T): Task;   // `async` is implementation-side and does not
                                                //   appear in a signature (spec §28.12)

  public method Add(scoped handler: (T) -> Task);        // scoped (v0.180): Add stores — spec §28.14
  public method Remove(handler: (T) -> Task);             // call-bounded: Remove only compares
  destructor;    // severs the subscriber list before reclamation (spec §28.14 quiescence)
end AsyncEventSource;
