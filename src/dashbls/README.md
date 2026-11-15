> [!NOTE]
>
> This is the copy of [dashpay/bls-signatures](https://github.com/dashpay/bls-signatures) vendored in Dash Core. It is built
> by `src/Makefile.dashbls.include`, with blst taken from `depends` or from the system. The bindings and the standalone
> build files are not part of this copy.

> [!WARNING]
>
> It is heavily advised **against** using this library for new consensus implementations and to use established
> spec-conformant libraries like [supranational/blst](https://github.com/supranational/blst) as this library codifies
> primitives predating the final IETF spec and includes a non-standard (now legacy) scheme.
>
> **This library has not undergone a formal security review.**

`bls-signatures` is a cross-platform library implementing BLS12-381 primitives for Dash built on
the [`blst library`](https://github.com/supranational/blst.git) toolkit.

## BLST license

BLST is used with the
[Apache 2.0 license](https://github.com/supranational/blst/blob/master/LICENSE)

## License

```text
Copyright (c) 2018-present, Chia Network, Inc.
Copyright (c) 2021-present, The Dash Core developers.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

The above license terms are available in [`LICENSE`](./LICENSE) and licensing terms for dependencies are
available in [`NOTICE`](./NOTICE)
