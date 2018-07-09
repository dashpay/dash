PGP keys
========

The file `keys.txt` contains fingerprints of the public keys of Gitian builders
and active developers.

The associated keys are mainly used to sign git commits or the build results
of Gitian builds.

You can import the keys into gpg as follows. Also, make sure to fetch the
latest version from the key server to see if any key was revoked in the
meantime.

```sh
gpg --import ./*.pgp
gpg --refresh-keys
```
