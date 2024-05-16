local skynet = require "skynet"
local crypt = require "skynet.crypt"

local function bin_to_hex(bin)
    local hex = {}
    for i = 1, #bin do
        hex[#hex + 1] = string.format("%02X", string.byte(bin, i))
    end
    return table.concat(hex)
end


skynet.start(function()
    local a = "aaaabbbb"

    local b = "ccccdddd"

    local exchange = crypt.dhexchange(a)
    print("交换后值", type(exchange), #exchange, bin_to_hex(exchange),  crypt.base64encode(exchange))

    local secret = crypt.dhsecret(a, b)
    print("秘密", type(secret), #secret,bin_to_hex(secret),  crypt.base64encode(secret))

    local hmac = crypt.hmac64_md5(a, b) -- b是key
    print("校验值", type(hmac), #hmac, bin_to_hex(hmac),  crypt.base64encode(hmac))

end)
