# Assets and licenses

Self-authored study code and documentation use the repository's Apache-2.0 license, copied as `LICENSE`. BugsJS, Express, Hexo and Hessian.js source retains its original MIT notices under `third_party` and embedded source headers. Node notices and official SHA256 lists are retained; Node executables are downloaded separately. No new license is granted for model weights, the inference engine or Epic software. Dependency recipes resolve third-party packages under their own notices; package caches are not redistributed.

Engine required by the study: ninfer-serve.exe,178712576 bytes, SHA256 `5aee4add582ea724d8aa2324e84287aa0a78894e55767d608ea9a2435ed8f292`.

MSE-labelled model required:14767307264 bytes, SHA256 `070bc339c272a38d7752f2b4a95a072724e56a5284c859bc80704b066cb6b852`.

The repository's public provenance record binds the experimental engine hash to build base commit6e8d41e685a16a58b94b199ac6b25fd02b67a241 with uncommitted source changes. An exact frozen build-source snapshot remains unattested; the clean upstream snapshot is a different source reference. The existing published product model and both published runtimes have hashes different from the study model/engine, as recorded in qualification/existing_public_asset_compatibility.json. Exact matching MSE artifact and engine public distribution are not established by this code/data deposit. A reader who lacks them can audit evidence and qualify six references with --references-only on the stated Windows/NVIDIA scaffold, but cannot reproduce original GPU trajectories from this directory alone. UE/native v7 execution also needs a separately licensed matching UE installation; v10 has no UE tasks.

Excluded: weights, engine/Node executables, UE SDK text and binaries, dependency caches, author declarations and unfinished manuscripts. Original runtime identities and omitted asset hashes remain in the manifest/provenance. A source file marked as a path-redacted derivative must not be presented as byte-identical to its original frozen counterpart.
