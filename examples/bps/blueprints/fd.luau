return function(ctx)
    return ctx.lib.fetchGitHubRelease {
        name = "fd",
        owner = "sharkdp",
        repo = "fd",
        version = "10.2.0",
        assetPattern = "x86_64%-pc%-windows%-msvc",
        sha256 = "1111111111111111111111111111111111111111111111111111111111111111",
        bin = "fd.exe",
    }
end
