// No-result form.
public static method WhenEach(tasks: Array<Task>): IAsyncEnumerable<Task>;

// Value-typed, string, or runtime-managed result (Pattern A). Each element is an
// already-completed Task<T>; await it to observe the result or re-raise its fault.
public static method WhenEach<T>(tasks: Array<Task<T>>): IAsyncEnumerable<Task<T>>;

// Owning overload for class T (Pattern B; v0.250 — same name, the parameter type
// distinguishes it, the call site is Task.WhenEach(move tasks)). Takes ownership
// of the array and every task in it; yields each completed OwningTask<T> as an
// owned iteration variable.
public static method WhenEach<T>(
    owned tasks: OwningArray<OwningTask<T>>
): IOwningAsyncEnumerable<OwningTask<T>> where T: class;
