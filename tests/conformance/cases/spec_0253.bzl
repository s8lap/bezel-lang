class TaskFactoryDemo
  async method Consume(): Task
    var i := await Task.FromResult(42);                                // T = int, value-typed
    var s := await Task.FromResult("done");                            // T = string, runtime-managed
    using var m := await Task.FromOwnedResult<MyClass>(new MyClass());  // T = MyClass → OwningTask<MyClass>
  end Consume;
end TaskFactoryDemo;
