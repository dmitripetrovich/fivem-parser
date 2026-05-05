local parsingEnabled = {}

RegisterCommand('toggleparser', function(source)
        if source == 0 then return end
        parsingEnabled[source] = not parsingEnabled[source]
        if not parsingEnabled[source] then
                TriggerClientEvent('fivem-parser:setState', source, false)
                return
        end
        TriggerClientEvent('fivem-parser:setState', source, true)
end, false)

AddEventHandler('playerDropped', function()
        parsingEnabled[source] = nil
end)
