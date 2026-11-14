# blst [![Lint Status](https://github.com/supranational/blst/workflows/golang-lint/badge.svg)](https://github.com/supranational/blst/actions/workflows/golang-lint.yml)

The `blst` package provides a Go interface to the blst BLS12-381 signature library.

## Build
The build process consists of two steps, code generation followed by compilation.

```
./generate.py # Optional - only required if making code changes
go build
go test
```

The generate.py script is used to generate both min-pk and min-sig variants of the binding from a common code base. It consumes the `*.tgo` files along with `blst_minpk_test.go` and produces `blst.go` and `blst_minsig_test.go`. The .tgo files can treated as if they were .go files, including the use of gofmt and goimports. The generate script will filter out extra imports while processing and automatically run goimports on the final blst.go file.

After running generate.py, <nobr>`go build`</nobr> and <nobr>`go test`</nobr> can be run as usual. Cgo will compile `cgo_server.c`, which includes the required C implementation files, and `cgo_assembly.S`, which includes appropriate pre-generated assembly code for the platform.

#### Caveats

If the test or target application crashes with an "illegal instruction" exception [after copying to an older system], rebuild with `CGO_CFLAGS` environment variable set to <nobr>`-O2 -D__BLST_PORTABLE__`</nobr>. Don't forget <nobr>`-O2`</nobr>!

On Windows the C compiler invoked by cgo, one denoted in `go env CC` output, has to target [MinGW](https://www.mingw-w64.org/). Verify with `<go-env-CC-output> -dM -E -x c nul: | findstr "MINGW64"`.

If you're cross-compiling, you have to set `CC` environment variable to the target C cross-compiler and `CGO_ENABLED` to 1. For example, to compile the test program for ARM:
```
env GOARCH=arm CC=arm-linux-gnueabi-gcc CGO_ENABLED=1 go test -c
```

## Usage
There are two primary modes of operation that can be chosen based on type definitions in the application.

For minimal-pubkey-size operations the application would define core types as:
```
type PublicKey = blst.P1Affine
type Signature = blst.P2Affine
type AggregateSignature = blst.P2Aggregate
type AggregatePublicKey = blst.P1Aggregate
```

For minimal-signature-size operations:
```
type PublicKey = blst.P2Affine
type Signature = blst.P1Affine
type AggregateSignature = blst.P1Aggregate
type AggregatePublicKey = blst.P2Aggregate
```

A complete example for generating a key, signing a message, and verifying the message:
```
package main

import (
	"crypto/rand"
	"fmt"

	blst "github.com/supranational/blst/bindings/go"
)

type PublicKey = blst.P1Affine
type Signature = blst.P2Affine
type AggregateSignature = blst.P2Aggregate
type AggregatePublicKey = blst.P1Aggregate

func main() {
	var ikm [32]byte
	_, _ = rand.Read(ikm[:])
	sk := blst.KeyGen(ikm[:])
	pk := new(PublicKey).From(sk)

	var dst = []byte("BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_NUL_")
	msg := []byte("hello foo")
	sig := new(Signature).Sign(sk, msg, dst)

	if !sig.Verify(true, pk, true, msg, dst) {
		fmt.Println("ERROR: Invalid!")
	} else {
		fmt.Println("Valid!")
	}
}
```

See the tests for further examples of usage.

## Core Methods

### SecretKey Methods
- `KeyGen(ikm []byte, optional ...[]byte) *SecretKey` - Derive the secret key scalar from secret input key material, optionally application-specific
- `Serialize() []byte` - Serialize the secret key to bytes
- `Deserialize(data []byte) *SecretKey` - Deserialize secret key from bytes
- `Zeroize()` - Securely zero out the secret key

### PublicKey (P1Affine in minimal-pubkey-size) Methods
- `From(sk *SecretKey) *PublicKey` - Derive public key from secret key
- `Compress() []byte` - Serialize public key to compressed format
- `Uncompress(data []byte) *PublicKey` - Decompress public key from bytes
- `Serialize() []byte` - Serialize public key to uncompressed format
- `Deserialize(data []byte) *PublicKey` - Deserialize public key from bytes

### Signature (P2Affine in minimal-pubkey-size) Methods
- `Sign(sk *SecretKey, msg []byte, dst []byte, ...interface{}) *Signature` - Sign a message
- `Compress() []byte` - Serialize signature to compressed format
- `Uncompress(data []byte) *Signature` - Decompress signature from bytes
- `BatchUncompress(compressedSigs [][]byte) []*Signature` - Efficiently uncompress multiple signatures
- `Serialize() []byte` - Serialize signature to uncompressed format
- `Deserialize(data []byte) *Signature` - Deserialize signature from bytes
- `Verify(sigCheck bool, pk *PublicKey, pkCheck bool, msg []byte, dst []byte, ...interface{}) bool` - Verify a signature
- `VerifyCompressed(sig []byte, sigCheck bool, pk []byte, msgCheck bool, msg []byte, dst []byte, ...interface{}) bool` - Verify a serialized signature in compressed format
- `AggregateVerify(sigCheck bool, pks []*PublicKey, msgCheck bool, msgs [][]byte, dst []byte) bool` - Verify an aggregated signature for multiple messages
- `AggregateVerifyCompressed(sig []byte, sigCheck bool, pks [][]byte, msgCheck bool, msgs [][]byte, dst []byte) bool` - Verify an aggregated serialized signature in compressed format
- `FastAggregateVerify(sigCheck bool, pks []*PublicKey, msg []byte, dst []byte) bool` - Fast verify for same message
- `MultipleAggregateVerify(sigs []*Signature, sigCheck bool, pks []*PublicKey, msgCheck bool, msgs [][]byte, dst []byte, randFn func(*Scalar), randBits int) bool` - Verify multiple signatures

### Aggregate Methods
- `AggregatePublicKey.Aggregate(pks []*PublicKey, check bool)` - Aggregate multiple public keys
- `AggregateSignature.Aggregate(sigs []*Signature, check bool)` - Aggregate multiple signatures
- `AggregateSignature.AggregateCompressed(compressedSigs [][]byte, check bool)` - Aggregate multiple serialized signatures in compressed format
- `AggregatePublicKey.ToAffine() *PublicKey` - Convert aggregate to affine form
- `AggregateSignature.ToAffine() *Signature` - Convert aggregate to affine form

## Utility Functions
- `HashToG1(msg []byte, dst []byte, optional... []byte) *P1` - Hash message [with optional augmentation] to G1 point
- `HashToG2(msg []byte, dst []byte, optional... []byte) *P2` - Hash message [with optional augmentation] to G2 point
- `P1Generator() *P1` - Get G1 generator point
- `P2Generator() *P2` - Get G2 generator point
- `Uniq(msgs [][]byte)` - Check messages for uniqueness
- `SetMaxProcs(procs int)` - Set maximum number of threads for parallel operations
