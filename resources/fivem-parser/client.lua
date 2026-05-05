local parsingEnabled = false

RegisterNetEvent('fivem-parser:setState', function(state)
        if type(state) ~= 'boolean' then return end
        parsingEnabled = state
end)

local function sanitize(str)
        if type(str) ~= 'string' then return nil end
        return (str:gsub('%c', ''))
end

local function log(author, text)
        text = sanitize(text)
        author = sanitize(author)
        if not text or text == '' then return end
        if not author or author == '' then print(('%s %s'):format('[chat]', text)) return end
        print(('%s %s: %s'):format('[chat]', author, text))
end

local function extract(message)
        if type(message) == 'string' then return nil, message end
        if type(message) ~= 'table' or type(message.args) ~= 'table' then return nil, nil end
        local first = message.args[1]
        local second = message.args[2]
        if not second or second == '' then return nil, first end
        return first, second
end

RegisterNetEvent('chat:addMessage', function(message)
        if not parsingEnabled then return end
        local author, text = extract(message)
        log(author, text)
end)

RegisterNetEvent('chatMessage', function(author, _, text)
        if not parsingEnabled then return end
        log(author, text)
end)
