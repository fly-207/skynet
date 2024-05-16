local skynet = require "skynet"
local crypt = require "skynet.crypt"

skynet.start(function()

    skynet.newservice("debug_console", 9999)

    skynet.newservice("crypt_test_sub")

end)
