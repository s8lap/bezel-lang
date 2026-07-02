using var src := new OwningTaskCompletionSource<MyClass>();
src.SetException(ex);
return src.Task;       // typed as OwningTask<MyClass>
