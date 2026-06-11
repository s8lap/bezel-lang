public method ReplaceResource(owned resource: Stream)
  _resource.Dispose();
  _resource := resource;
end ReplaceResource;
