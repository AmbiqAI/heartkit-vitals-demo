# Contributing

Open an issue before starting a change so maintainers can confirm scope. Keep
pull requests focused and link the issue they address.

## Development and validation

See [developer setup](docs/developer.md) for toolchain and board instructions.
Firmware builds require authorized access to the private AS7058 driver module
pinned in `nsx.lock`. Public repository access does not grant driver access.
Use the prebuilt release packages if you do not have that access.

The standalone host tests do not require the sensor driver:

```sh
cmake -S tests -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

In your PR, distinguish host tests, firmware builds, and actual board tests.
Name the board and transport tested. Build boards sequentially as described in
the developer guide. Fork PRs cannot access private dependency credentials;
maintainers must arrange firmware validation before merge. Never place access
tokens or private driver source in a contribution.

## Licensing

Preserve applicable copyright and license notices. Identify the source and
redistribution terms of added code, models, data, and images. This repository
has mixed licensing; see [LICENSE](LICENSE), [NOTICE](NOTICE), and
[third-party notices](THIRD-PARTY-NOTICES.md). Generated AOT modules have their
own hardware-restricted license and are not covered by the root BSD license.

Report suspected vulnerabilities privately as described in [SECURITY.md](SECURITY.md).
