-- A leaf blueprint: fetch one GitHub-release tool.
return function(ctx)
    return ctx.lib.fetchGitHubRelease {
        name = "ripgrep",
        owner = "BurntSushi",
        repo = "ripgrep",
        version = "14.1.0",
        assetPattern = "x86_64%-pc%-windows%-msvc",
        sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
        bin = "rg.exe",
    }
end
