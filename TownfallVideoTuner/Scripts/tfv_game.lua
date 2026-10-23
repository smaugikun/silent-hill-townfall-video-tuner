local UEHelpers = require("UEHelpers")
local native = require("tfv_native")
local screen = require("tfv_screen")
local M = {}
local valid = native.valid
local controller, pawn, world
local worldAddress, pawnAddress, epoch = nil,nil,0
local sequences, sources, intervals, enemies = {},{},{},{}
local lastReserve = -math.huge
local reservationValid=false
local playerClass, statics, waypointClass
-- The game's own CRTV videos and transmissions are waypoints: the stations take their tuning range (the most
-- common one in the level), 0 while there are none. isWaypoint: by source address, asked once each.
local waypointTolerance, isWaypoint = 0, {}

local function watch(path, list)
    local class = StaticFindObject(path)
    if valid(class) then NotifyOnNewObject(path,function(object)
        if valid(object) then list()[#list()+1]=object end
    end) end
    return class
end
function M.init()
    playerClass=StaticFindObject("/Script/Townfall.TownfallPlayerCharacter")
    statics=StaticFindObject("/Script/Engine.Default__GameplayStatics")
    if not (valid(playerClass) and valid(statics)) then
        native.log("game", "Game reflection unavailable; native stations will yield")
        return false
    end
    watch("/Script/LevelSequence.LevelSequencePlayer",function() return sequences end)
    waypointClass=watch("/Script/Townfall.RadioWaypointSourceComponent",function() return sources end)
    watch("/Script/Townfall.RadioStaticSourceComponent",function() return sources end)
    watch("/Script/Townfall.TownfallEnemyCharacter",function() return enemies end)
    return true
end
local function newLevel()
    epoch=epoch+1
    screen.reset()
    sequences,sources,enemies,intervals={},{},{},{}
    waypointTolerance,isWaypoint=0,{}
    lastReserve=-math.huge; reservationValid=false
    native.log("world", "World/pawn changed; media state and CRTV ownership reset (epoch "..epoch..")")
    -- Splash/menu pawns do not need source scans across the game's object array.
    if not (valid(pawn) and valid(playerClass) and pawn:IsA(playerClass)) then return end
    sequences=FindAllOf("LevelSequencePlayer") or {}
    sources=FindAllOf("RadioWaypointSourceComponent") or {}
    for _,object in ipairs(FindAllOf("RadioStaticSourceComponent") or {}) do sources[#sources+1]=object end
    -- Enemy Blueprints can own a subclass of RadioStaticSourceComponent.
    enemies=FindAllOf("TownfallEnemyCharacter") or {}
    for _,enemy in ipairs(enemies) do
        if valid(enemy) then
            local ok,source=pcall(function() return enemy.RadioStaticSource end)
            if ok and valid(source) then sources[#sources+1]=source end
        end
    end
end
local function reserved(frequency)
    local now=os.clock()
    if now-lastReserve>=0.25 then
        lastReserve=now
        local livingEnemies={}
        for _,enemy in ipairs(enemies) do if valid(enemy) then
            livingEnemies[#livingEnemies+1]=enemy
            local ok,source=pcall(function() return enemy.RadioStaticSource end)
            if ok and valid(source) then sources[#sources+1]=source end
        end end
        enemies=livingEnemies
        local alive,seen,counts={},{},{}
        intervals={}
        for _,source in ipairs(sources) do
            if valid(source) then
                local address=source:GetAddress()
                if not seen[address] then
                    seen[address]=true; alive[#alive+1]=source
                    -- FindAllOf also finds Blueprint templates. Only placed components in
                    -- this player's world reserve channels; templates have no actor owner.
                    local owner=source:GetOwner()
                    local sourceWorld=valid(owner) and owner:GetWorld() or nil
                    if valid(sourceWorld) and valid(world) and sourceWorld:GetAddress()==world:GetAddress() then
                        local ok,f,t=pcall(function() return source.TuningFrequency,source.TuningTolerance_Outer end)
                        if not ok or type(f)~="number" or type(t)~="number" or f~=f or t~=t or t<0 then
                            reservationValid=false
                            return true
                        end
                        if f>=0 and f<=1 and t>=0 then intervals[#intervals+1]={f-t,f+t} end
                        if isWaypoint[address]==nil then
                            isWaypoint[address]=valid(waypointClass) and source:IsA(waypointClass) or false
                            if isWaypoint[address] then
                                local inner=select(2,pcall(function() return source.TuningTolerance_Inner end))
                                native.log("signal "..address,string.format("CRTV signal at %.4f: comes in within %.4f, clear within %s",
                                    f,t,type(inner)=="number" and string.format("%.4f",inner) or "?"))
                            end
                        end
                        if isWaypoint[address] and t>0 then counts[t]=(counts[t] or 0)+1 end
                    end
                end
            end
        end
        sources=alive; reservationValid=true
        local best=0
        for t,n in pairs(counts) do if n>(counts[best] or 0) or (n==counts[best] and t>best) then best=t end end
        waypointTolerance=best
        local playingAlive={}
        for _,sequence in ipairs(sequences) do if valid(sequence) then playingAlive[#playingAlive+1]=sequence end end
        sequences=playingAlive
    end
    if not reservationValid then return true end
    for _,range in ipairs(intervals) do if frequency>=range[1] and frequency<=range[2] then return true end end
    return false
end
-- Whether the player's CRTV is up, from as little as the game can tell it: main.lua asks only this on idle frames.
function M.radioUp()
    local current=valid(controller) and controller.Pawn or nil
    if not valid(current) then return false end
    local ok,active=pcall(function() return current:GetIsRadioInActiveMode() end)
    return ok and active==true
end
function M.read()
    local previousWorldValid=valid(world)
    if not valid(controller) then controller=UEHelpers.GetPlayerController() end
    local nextPawn=valid(controller) and controller.Pawn or nil
    local nextWorld=valid(nextPawn) and nextPawn:GetWorld() or nil
    local nextWorldAddress=valid(nextWorld) and nextWorld:GetAddress() or 0
    local nextPawnAddress=valid(nextPawn) and nextPawn:GetAddress() or 0
    if not previousWorldValid or worldAddress~=nextWorldAddress or pawnAddress~=nextPawnAddress then
        -- Do not scan all UObjects every menu tick or while loading.
        if worldAddress~=nextWorldAddress or pawnAddress~=nextPawnAddress then
            world,pawn=nextWorld,nextPawn; worldAddress,pawnAddress=nextWorldAddress,nextPawnAddress
            newLevel()
        end
    end
    world,pawn=nextWorld,nextPawn
    local state={active=false,priority=true,paused=false,frequency=0,world=world,epoch=epoch,
                 reservationValid=false,ranges={},gameplay=false}
    if not (valid(pawn) and valid(world) and valid(playerClass) and pawn:IsA(playerClass)) then return state end
    local ok,dead=pcall(function() return pawn:IsDead() end)
    if ok and dead then return state end
    if type(pawn.Health)=="number" and pawn.Health<=0 then return state end
    state.gameplay=true
    state.paused=statics:IsGamePaused(pawn)
    state.active=pawn:GetIsRadioInActiveMode()
    -- Publish all placed game tuning ranges before the first dial markers appear.
    -- This also warms reservations while the CRTV is lowered.
    reserved(0)
    state.reservationValid=reservationValid
    state.ranges=intervals
    state.waypointTolerance=waypointTolerance
    if not state.active then screen.reset(); return state end
    state.radio=pawn:GetRadio()
    if not valid(state.radio) then state.active=false; return state end
    state.frequency=pawn:GetRadioCurrentTunedFrequency()
    if type(state.frequency)~="number" or state.frequency~=state.frequency then state.active=false; return state end
    state.priority=reserved(state.frequency)
    -- Fine tuning, active game signals and signal-object mode are also checked natively every game tick.
    for _,sequence in ipairs(sequences) do
        if valid(sequence) and sequence:IsPlaying() then state.priority=true; break end
    end
    return state
end
return M
