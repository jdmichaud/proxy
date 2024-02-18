const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const liburing_dep = b.dependency("liburing", .{
      .target = target,
      .optimize = optimize,
    });

    const exe = b.addExecutable(.{
      .name = "proxy",
      .target = target,
      .optimize = optimize,
    });

    const flags: []const []const u8 = switch (optimize) {
      .Debug => &.{
        "-Wall",
        "-ggdb3",
        "-fsanitize=undefined",
      },
      .ReleaseSafe => &.{
        "-Wall",
        "-fsanitize=undefined",
      },
      .ReleaseFast => &.{
        "-Wall",
        "-O3",
      },
      .ReleaseSmall => &.{
        "-Wall",
        "-Os",
      },
    };

    exe.addCSourceFile(.{
      .file = .{
        .cwd_relative = "src/proxy.c",
      },
      .flags = flags,
    });
    exe.linkLibC();
    exe.linkLibrary(liburing_dep.artifact("uring"));
    exe.addIncludePath(liburing_dep.path("src/include"));
    b.installArtifact(exe);

    const run_cmd = b.addRunArtifact(exe);

    run_cmd.step.dependOn(b.getInstallStep());

    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run the app");
    run_step.dependOn(&run_cmd.step);
}
