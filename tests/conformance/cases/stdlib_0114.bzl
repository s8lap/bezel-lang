// No-result form.
public static method WhenEach(tasks: Array<Task>): IAsyncEnumerable<Task>;

// Value-typed, string, or runtime-managed result (Pattern A). Each element is an
// already-completed Task<T>; await it to observe the result or re-raise its fault.
public static method WhenEach<T>(tasks: Array<Task<T>>): IAsyncEnumerable<Task<T>>;

// Owning form for class T (Pattern B). Takes ownership of the array and every
// task in it; yields each completed OwningTask<T> as an owned iteration variable.
public static method WhenEachOwned<T>(
    owned tasks: OwningArray<OwningTask<T>>
): IOwningAsyncEnumerable<OwningTask<T>> where T: class;
