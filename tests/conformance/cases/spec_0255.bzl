public async method WaitForKeyPressAsync(): Task<char>
  using var tcs := new TaskCompletionSource<char>();

  Keyboard.OnKeyPress(lambda(key) => do
    tcs.TrySetResult(key);
  end);

  return await tcs.Task;
end WaitForKeyPressAsync;
