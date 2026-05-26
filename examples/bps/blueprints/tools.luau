-- A composite blueprint: pull two leaf tools via the lazy package set.
return function(ctx)
    return ctx.lib.composite {
        name = "tools",
        description = "handy CLI tools",
        components = { ctx.pkgs.ripgrep, ctx.pkgs.fd },
    }
end
