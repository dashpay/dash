const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const mod = b.addModule("blst", .{
        .root_source_file = b.path("bindings/zig/blst.zig"),
        .target = target,
        .optimize = optimize,
    });

    const cfiles = &[_][]const u8{
        "src/server.c",
        "build/assembly.S",
    };

    const cflags = &[_][]const u8{
        "-O2", "-ffreestanding", "-D__BLST_PORTABLE__",
        "-D__BLST_NO_ASM__",
    };

    switch (target.result.cpu.arch) {
        .aarch64,
        .x86_64  => mod.addCSourceFiles(.{
                        .files = cfiles,
                        .flags = cflags[0 .. cflags.len-1],
                    }),
        else     => mod.addCSourceFiles(.{
                        .files = cfiles[0 .. cfiles.len-1],
                        .flags = cflags,
                    }),
    }
    if (target.result.os.tag == .windows) {
        mod.link_libc = true;
    }

    const tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("bindings/zig/tests.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{.{ .name = "blst", .module = mod }},
        }),
    });

    b.step("test", "Run test[s]").dependOn(&b.addRunArtifact(tests).step);
}
