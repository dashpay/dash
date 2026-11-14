# blst for [Zig](https://ziglang.org/)

The object-oriented interface is modeled after [C++ interface](../blst.hpp), but at the time of writing is a subset of it, sufficient to produce and verify individual and aggregated signatures. See [tests.zig](tests.zig) for an example. C symbols are available with `blst.c.` prefix instead of `blst_`, e.g. `blst_miller_loop` is accessible as `blst.c.miller_loop`.

## Adding dependency to your project

Execute
```
zig fetch --save git+https://github.com/supranational/blst#v0.3.17
```
and add an equivalent of the following line to your build.zig prior to `b.installArtifact(exe)`:
```
exe.root_module.addImport("blst", b.dependency("blst", .{}).module("blst"));
```
You should now be able to `@import("blst")` in your application code. The abovementioned fetch command can be used to update the git reference.
