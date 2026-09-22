-- default config for the bear language server, picked up by vim.lsp.config / vim.lsp.enable
-- (neovim >= 0.11) once this directory is on the runtimepath
--
-- override any field with vim.lsp.config('bearls', { ... }) before calling vim.lsp.enable

---@type vim.lsp.Config
return {
    cmd = { 'bearls' },
    filetypes = { 'bear' },
    -- the directory of the first match above the file becomes the workspace folder, which bearls
    -- adds to bearc's import paths
    root_markers = { '.git' },
    -- read once, in initialize
    init_options = {
        -- extra -I directories, relative ones are resolved against the workspace folder
        importPaths = {},
        -- extra bearc flags, e.g. { '--warn-cyclic-import' }
        extraArgs = {},
        -- quiet period (ms) after an edit before open files are re-analyzed
        debounceMs = 30,
    },
}
