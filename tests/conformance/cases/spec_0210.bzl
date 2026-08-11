public method ReplaceResource(owned resource: Stream)
  _resource := resource;   // previous _resource disposed here, implicitly (§28.7)
end ReplaceResource;
