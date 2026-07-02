Task.FromResult(42);                               // T = int, value-typed
Task.FromResult("done");                           // T = string, runtime-managed
Task.FromOwnedResult<MyClass>(new MyClass());      // T = MyClass, returns OwningTask<MyClass>
