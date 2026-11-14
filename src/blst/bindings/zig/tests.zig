const std = @import("std");
const blst = @import("blst");

test "sign/verify" {
    const password = [_]u8{'*'} ** 32;

    var SK = blst.SecretKey{};
    SK.keygen(&password, null);
    defer SK.deinit();

    const msg = "assertion";
    const DST = "MY-DST";

    // on the "sender" side...

    const pk_for_wire  = (try blst.P1.from(&SK)).serialize();
    const sig_for_wire = blst.P2.hash_to(msg, DST, &pk_for_wire).sign_with(&SK).serialize();

    // ... and now on the "receiver" side...

    const sig = try blst.P2_Affine.from(&sig_for_wire);
    const pk  = try blst.P1_Affine.from(&pk_for_wire);

    const ret = sig.core_verify(&pk, true, msg, DST, &pk_for_wire);

    try std.testing.expectEqual(ret, .SUCCESS);
}

test "uniq" {
    const msgs = &[_][]const u8 {
        "three", "two", "one", "three",
    };

    var ctx = try blst.Uniq.init(msgs.len, std.testing.allocator);
    defer ctx.deinit();

    for (msgs, 1..) |msg, next| {
        try std.testing.expectEqual(ctx.is_uniq(msg), next < msgs.len);
    }
}

fn box(allocator: std.mem.Allocator, src: []const u8) ![]u8 {
    const ret = try allocator.alloc(u8, src.len);
    @memcpy(ret, src);
    return ret;
}

test "aggregateverify" {
    const mem = std.testing.allocator;
    const N = 3;

    const password = [_]u8{'*'} ** 32;
    var SK = blst.SecretKey{};
    defer SK.deinit();

    // emulate N "senders"...

    const DST = "MY-DST";
    var pks: [N][]const u8 = undefined;
    var sigs: [N][]const u8 = undefined;
    var msgs: [N][]const u8 = undefined;

    for (0..N) |i| {
        msgs[i] = try std.fmt.allocPrint(mem, "assertion{}", .{i});
        SK.keygen(&password, msgs[i]);
        pks[i] = try box(mem, &(try blst.P1.from(&SK)).serialize());
        sigs[i] = try box(mem, &blst.P2.hash_to(msgs[i], DST, null).sign_with(&SK).serialize());
    }

    // ... basic scheme on the "receiver" side.

    var uniq = try blst.Uniq.init(msgs.len, mem);
    defer uniq.deinit();

    // The basic scheme requires messages to be checked for uniqueness.
    for (0..N) |i| {
        try std.testing.expectEqual(uniq.is_uniq(msgs[i]), true);
    }

    var aggregated = try blst.P2.from(sigs[0]);
    try std.testing.expectEqual(aggregated.in_group(), true);
    for (1..N) |i| {
        try aggregated.aggregate(&try blst.P2_Affine.from(sigs[i]));
    }

    var ctx = try blst.Pairing.init(true, DST, mem);
    defer ctx.deinit();

    // The below .aggregate() method doesn't vet public keys with
    // rationale that application would cache the results of the
    // group checks. Hence they need to be vetted separately.
    var pk = try blst.P1_Affine.from(pks[0]);
    try std.testing.expectEqual(pk.in_group(), true);
    try std.testing.expectEqual(ctx.aggregate(&pk, &aggregated.to_affine(), msgs[0], null),
                                .SUCCESS);
    for (1..N) |i| {
        pk = try blst.P1_Affine.from(pks[i]);
        try std.testing.expectEqual(pk.in_group(), true);
        try std.testing.expectEqual(ctx.aggregate(&pk, null, msgs[i], null),
                                    .SUCCESS);
    }

    ctx.commit();
    try std.testing.expectEqual(ctx.finalverify(null), true);

    for (0..N) |i| {
        mem.free(pks[i]);
        mem.free(sigs[i]);
        mem.free(msgs[i]);
    }
}
