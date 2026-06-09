--!nonstrict
local Players = game:GetService("Players")
local player = Players.LocalPlayer

local UNIT_Z = Vector3.new(0,0,1)
local DEFAULT_DISTANCE = 12.5
local FIRST_PERSON_DISTANCE_THRESHOLD = 1.0

local BaseCamera = {}
BaseCamera.__index = BaseCamera

function BaseCamera.new()
	local self = setmetatable({}, BaseCamera)
	self.gamepadZoomLevels = {0, 10, 20}
	self.FIRST_PERSON_DISTANCE_THRESHOLD = FIRST_PERSON_DISTANCE_THRESHOLD
	self.cameraType = nil
	self.currentSubjectDistance = math.clamp(DEFAULT_DISTANCE, player.CameraMinZoomDistance, player.CameraMaxZoomDistance)
	self.inFirstPerson = false
	self.enabled = false
	return self
end

function BaseCamera:GetModuleName()
	return "BaseCamera"
end

function BaseCamera:GetCameraLookVector(): Vector3
	return game.Workspace.CurrentCamera and game.Workspace.CurrentCamera.CFrame.LookVector or UNIT_Z
end

function BaseCamera:GamepadZoomPress()
	local dist = self:GetCameraToSubjectDistance()
	local max = player.CameraMaxZoomDistance

	for i = #self.gamepadZoomLevels, 1, -1 do
		local zoom = self.gamepadZoomLevels[i]
		if max < zoom then
			continue
		end
		if zoom < player.CameraMinZoomDistance then
			zoom = player.CameraMinZoomDistance
			if max == zoom then
				break
			end
		end
		if dist > zoom + (max - zoom) / 2 then
			self:SetCameraToSubjectDistance(zoom)
			return
		end
		max = zoom
	end

	self:SetCameraToSubjectDistance(self.gamepadZoomLevels[#self.gamepadZoomLevels])
end

function BaseCamera:OnCurrentCameraChanged()
	if self.cameraSubjectChangedConn then
		self.cameraSubjectChangedConn:Disconnect()
		self.cameraSubjectChangedConn = nil
	end

	local camera = game.Workspace.CurrentCamera
	if camera then
		self.cameraSubjectChangedConn = camera:GetPropertyChangedSignal("CameraSubject"):Connect(function()
			self:OnNewCameraSubject()
		end)
		self:OnNewCameraSubject()
	end
end

function BaseCamera:GetCameraToSubjectDistance(): number
	return self.currentSubjectDistance
end

return BaseCamera
