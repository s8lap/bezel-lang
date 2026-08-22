public async method LoadResourceAsync(path: string): OwningTask<Resource>
  using var tcs := new OwningTaskCompletionSource<Resource>();

  StartLoadInBackground(path, lambda(owned loaded: Resource) => do
    tcs.TrySetResult(move loaded);                       // ownership moves into tcs's task
  end);

  return await tcs.Task;                            // moves ownership out of tcs's task
                                                    //   and into the outer Pattern B task's
                                                    //   result slot. tcs is then disposed,
                                                    //   but its task already has no result.
end LoadResourceAsync;
