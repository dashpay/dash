# MacOS Deployment

The `macdeployqtplus` script should not be run manually. Instead, after building as usual:

```bash
make deploy
```

When complete, it will have produced `Dash-Core.zip`.

## SDK Extraction

### Step 1: Obtaining `Xcode.app`

A free Apple Developer Account is required to proceed.

Our macOS SDK can be extracted from
[Xcode_16.xip](https://download.developer.apple.com/Developer_Tools/Xcode_16/Xcode_16.xip).

Alternatively, after logging in to your account go to 'Downloads', then 'More'
and search for [`Xcode 16`](https://developer.apple.com/download/all/?q=Xcode%2016).

An Apple ID and cookies enabled for the hostname are needed to download this.

The `sha256sum` of the downloaded XIP archive should be `4a26c3d102a55c7222fb145e0ee1503249c9c26c6e02dc64d783c8810b37b1e3`.

To extract the `.xip` on Linux:

```bash
# Install/clone tools needed for extracting Xcode.app
apt install cpio
git clone https://github.com/bitcoin-core/apple-sdk-tools.git

# Unpack the .xip and place the resulting Xcode.app in your current
# working directory
python3 apple-sdk-tools/extract_xcode.py -f Xcode_16.xip | cpio -d -i
```

On macOS:

```bash
xip -x Xcode_16.xip
```

### Step 2: Generating the SDK tarball from `Xcode.app`

To generate the SDK, run the script [`gen-sdk`](./gen-sdk) with the
path to `Xcode.app` (extracted in the previous stage) as the first argument.

```bash
./contrib/macdeploy/gen-sdk '/path/to/Xcode.app'
```

The generated archive should be: `Xcode-16.0-16A242d-extracted-SDK-with-libcxx-headers.tar.gz`.
The `sha256sum` should be `bce59aa16560f182e44200a0b9539bd637c8b5c7089fbff13b0712730ce162ff`.

## Deterministic macOS App Notes

macOS Applications are created on Linux using a recent LLVM.

All builds must target an Apple SDK. These SDKs are free to download, but not redistributable.
See the SDK Extraction notes above for how to obtain it.

The Guix build process has been designed to avoid including the SDK's files in Guix's outputs.
All interim tarballs are fully deterministic and may be freely redistributed.

Using an Apple-blessed key to sign binaries is a requirement to produce (distributable) macOS
binaries. Because this private key cannot be shared, we'll have to be a bit creative in order
for the build process to remain somewhat deterministic. Here's how it works:

- Builders use Guix to create an unsigned release. This outputs an unsigned ZIP which
  users may choose to bless, self-codesign, and run. It also outputs an unsigned app structure
  in the form of a tarball.
- The Apple keyholder uses this unsigned app to create a detached signature, using the
  included script. Detached signatures are available from this [repository](https://github.com/dashpay/dash-detached-sigs).

- Builders feed the unsigned app + detached signature back into Guix, which combines the
  pieces into a deterministic ZIP.
