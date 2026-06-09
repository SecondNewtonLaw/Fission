--!strict
--[[
    Utility for handing LocalPlayer, Character and instances under Character (including Humanoid)
    This is a static class.
--]]

local Players = game:GetService("Players")

local CommonUtils = script.Parent
local ConnectionUtil = require(CommonUtils:WaitForChild("ConnectionUtil"))

local CONNECTIONS = {
    LOCAL_PLAYER = "LOCAL_PLAYER",
    ON_LOCAL_PLAYER = "ON_LOCAL_PLAYER",
    CHARACTER_ADDED = "CHARACTER_ADDED",
    ON_CHARACTER = "ON_CHARACTER",
    CHARACTER_CHILD_ADDED = "CHARACTER_CHILD_ADDED",
}
export type CharacterUtilClass = {
    getLocalPlayer: () -> Player?,
    onLocalPlayer: (func: (Player) -> ()) -> RBXScriptConnection,
    getCharacter: () -> Model?,
    onCharacter: (func: (Model) -> ()) -> RBXScriptConnection,
    getChild: (name: string, className: string) -> Instance?,
    onChild: (name: string, className: string, func: (Instance) -> ()) -> RBXScriptConnection,
    _connectionUtil: ConnectionUtil.ConnectionUtil,
    _boundEvents: {[string]: BindableEvent},
    _getOrCreateBoundEvent: (name: string) -> BindableEvent,
}

local CharacterUtil: CharacterUtilClass = {} :: CharacterUtilClass

CharacterUtil._connectionUtil = ConnectionUtil.new()
CharacterUtil._boundEvents = {}

function CharacterUtil.getLocalPlayer()
    return Players.LocalPlayer
end

function CharacterUtil.onLocalPlayer(func)
    local localPlayer = CharacterUtil.getLocalPlayer()
    if localPlayer then
        func(localPlayer)
    end

	CharacterUtil._connectionUtil:trackConnection(
		CONNECTIONS.LOCAL_PLAYER,
		Players:GetPropertyChangedSignal("LocalPlayer"):Connect(function()
			local localPlayer = CharacterUtil.getLocalPlayer()
			assert(localPlayer)
			CharacterUtil._getOrCreateBoundEvent(CONNECTIONS.LOCAL_PLAYER):Fire(localPlayer)
		end)
	)

    local boundEvent = CharacterUtil._getOrCreateBoundEvent(CONNECTIONS.LOCAL_PLAYER)
    return boundEvent.Event:Connect(func)
end

function CharacterUtil.getCharacter()
    local localPlayer = CharacterUtil.getLocalPlayer()
    if not localPlayer then
        return nil
    end
    return localPlayer.Character
end

function CharacterUtil.getChild(name: string, className: string)
    local character = CharacterUtil.getCharacter()
    if not character then
        return nil
    end
    local child = character:FindFirstChild(name)
    if child and child:IsA(className) then
        return child
    end
    return nil
end

function CharacterUtil._getOrCreateBoundEvent(name: string)
    if not CharacterUtil._boundEvents[name] then
        CharacterUtil._boundEvents[name] = Instance.new("BindableEvent")
    end
    return CharacterUtil._boundEvents[name]
end

return CharacterUtil
