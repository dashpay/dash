### Verify Binaries

#### Usage:

This script attempts to download the signature file `SHA256SUMS.asc` from GitHub releases (`https://github.com/dashpay/dash/releases`) and `https://dashcore-binaries.thepasta.org` and compares them.

It first checks if the signature passes, and then downloads the files specified in the file, and checks if the hashes of these files match those that are specified in the signature file.

The script returns 0 if everything passes the checks. It returns 1 if either the signature check or the hash check doesn't pass. If an error occurs the return value is >= 2.


```sh
./verify.py 21.1.1
./verify.py 22.0.0
./verify.py 22.1.0-rc.1
```

If you only want to download the binaries of certain platform, add the corresponding suffix, e.g.:

```sh
./verify.py 21.1.1-darwin
./verify.py 22.0.0-linux
./verify.py 22.1.0-rc.1-win64
```

If you do not want to keep the downloaded binaries, specify anything as the second parameter.

```sh
./verify.py 22.0.0 delete
```
