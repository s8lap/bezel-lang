using var src := new OwnedTaskCompletionSource<MyClass>();
src.SetException(ex);
return src.Task;       // typed as OwnedTask<MyClass>
