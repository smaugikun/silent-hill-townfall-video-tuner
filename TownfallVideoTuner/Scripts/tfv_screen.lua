local native = require("tfv_native")
local M = {}
local valid = native.valid
local owned, blocked
local function unwrap(value) return value and value.get and value:get() or value end

function M.restore()
    if owned then
        if valid(owned.radio) and valid(owned.material) and valid(owned.original) and valid(owned.target) then
            local current = owned.material:K2_GetTextureParameterValue(owned.parameter)
            if valid(current) and current:GetAddress()==owned.target:GetAddress() then
                owned.material:SetTextureParameterValue(owned.parameter,owned.original)
                native.log("texture", "Original CRTV texture restored")
            end
        else native.log("texture", "CRTV objects invalidated; old texture references dropped") end
    end
    owned = nil
end
function M.reset() M.restore(); blocked=nil end
function M.isBound() return owned~=nil end
function M.inspect(radio, frequency, target)
    if not valid(radio) then M.restore(); return nil end
    if blocked then
        if blocked.radio==radio:GetAddress() and math.abs(frequency-blocked.frequency)<=0.03 then return nil end
        blocked=nil
    end
    local widget = radio:GetCRTVWidget()
    if not valid(widget) then M.restore(); return nil end
    local setup = unwrap(radio.RadarPropMap:Find(radio.PlayerCharacterType))
    local radar = setup and setup.RadarTexture
    if not (radar and valid(radar.Widget) and valid(radar.Material)) then M.restore(); return nil end
    if radar.Widget:GetAddress()~=widget:GetAddress() then M.restore(); return nil end
    local material,parameter = radar.Material,radar.MaterialTextureName
    if owned and (not valid(owned.material) or not valid(owned.radio) or
        owned.radio:GetAddress()~=radio:GetAddress() or owned.material:GetAddress()~=material:GetAddress()) then M.restore() end
    local current = material:K2_GetTextureParameterValue(parameter)
    if not valid(current) then M.restore(); return nil end
    if owned and current:GetAddress()~=target:GetAddress() then
        -- The game changed the screen while we owned it. Preserve the game's new binding.
        owned=nil; blocked={radio=radio:GetAddress(),frequency=frequency}
        native.log("texture", "Game changed CRTV material; yielding until tuning moves or CRTV closes")
        return nil
    end
    if not owned and current:GetAddress()==target:GetAddress() then
        -- Recover the widget's actual render target after a Lua reload, when available.
        local ok,original = pcall(function() return widget:GetRenderTarget() end)
        if not (ok and valid(original) and original:GetAddress()~=target:GetAddress()) then return nil end
        material:SetTextureParameterValue(parameter,original)
        current=original
    end
    local original = owned and owned.original or current
    if not valid(original) then M.restore(); return nil end
    local width,height = 640,512
    local ok,x,y = pcall(function() return original.SizeX,original.SizeY end)
    if ok and type(x)=="number" and type(y)=="number" and x>=64 and x<=2048 and y>=64 and y<=2048 then width,height=x,y end
    return {material=material,parameter=parameter,original=original,radio=radio,target=target,width=width,height=height}
end
function M.bind(screen)
    if not screen then M.restore(); return false end
    if owned then return true end
    if not (valid(screen.material) and valid(screen.original) and valid(screen.target) and valid(screen.radio)) then return false end
    -- A setter followed by readback prevents audio playing while the screen is still the game's.
    screen.material:SetTextureParameterValue(screen.parameter,screen.target)
    local current = screen.material:K2_GetTextureParameterValue(screen.parameter)
    if valid(current) and current:GetAddress()==screen.target:GetAddress() then
        owned=screen
        native.log("texture", "Video output bound to the existing CRTV screen material")
        return true
    end
    return false
end
return M
