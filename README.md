# SPITE Engine

The open part of the [SPITE engine](https://spite-engine.com): the source of the open engine modules
(`source/open`), the public API headers (`include/spite`), and the developer SDK's
documentation, samples and tooling (`sdk/DeveloperSdk`). The engine core is
proprietary and ships only as binaries in the SPITE SDK.

## Licence

Everything in this repository is licensed under the [Apache License 2.0](LICENSE);
see [NOTICE](NOTICE). Third-party code is not copied here: `source/external` holds
git submodules pinned to the exact upstream commits SPITE builds with, each under
its own licence (`THIRD_PARTY_NOTICES.md` lists them).

The SDK archives attached to this repository's releases contain the proprietary
engine core and are licensed under the [SPITE Engine licence](https://github.com/spite-engine-global/spite-engine/releases/latest/download/SPITE-ENGINE-LICENSE.txt), not under
Apache 2.0. Commercial game development and distribution are permitted under that licence;
rights to the proprietary core remain reserved to the developers of SPITE Engine.

## Getting the source

```
git clone --recursive https://github.com/spite-engine-global/spite-engine.git
```

or, in an existing clone, `git submodule update --init`.

## Building

The open modules build against the binary SPITE SDK of the same version: each
release tag here matches the SDK archive attached to that release. Start with
`sdk/DeveloperSdk/developer/README.md`.

## Contributing

This repository is generated from the SPITE engine repository at each release.
Pull requests are welcome; an accepted change is applied in the engine repository
and appears here with the next release. Contributions are accepted under the
Apache License 2.0 (section 5).
