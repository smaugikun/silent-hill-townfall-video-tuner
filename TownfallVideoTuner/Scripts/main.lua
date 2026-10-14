-- Game-thread reflection only. All decoding, playback, rendering and audio are native.
local native = require("tfv_native")
local screen = require("tfv_screen")
local game = require("tfv_game")
local valid=native.valid
if _G.__tf_video_shutdown then pcall(_G.__tf_video_shutdown) end
if not native.init() then return end
if not game.init() then native.close(); return end

local mediaOuter,mediaPackagePath
local function mediaPackage()
    if valid(mediaOuter) then return mediaOuter end
    -- The class already belongs to a loaded UPackage. Get that actual object
    -- directly: startup-time enumeration does not expose the transient package
    -- reliably in this game. Our four objects still have RF_Transient/root flags.
    local class=StaticFindObject("/Script/MediaAssets.MediaPlayer")
    if not valid(class) then error("Runtime MediaPlayer class unavailable") end
    local outer=class:GetOuter()
    local path=valid(outer) and outer:GetFullName():match("^Package (.+)$")
    if not path then error("Runtime MediaAssets package unavailable") end
    mediaOuter,mediaPackagePath=outer,path
    return mediaOuter
end
local function mediaObject(path,name)
    local class=StaticFindObject(path)
    local outer=mediaPackage()
    if not (valid(class) and valid(outer)) then error("Runtime media class/outer not available: "..path) end
    local existing=StaticFindObject(mediaPackagePath.."."..name)
    if valid(existing) then
        if not existing:IsA(class) then error("Media object name belongs to another class: "..name) end
        return existing
    end
    -- RF_Transient | RF_MarkAsRootSet = 64 | 128, verified from this game's PDB.
    local object=StaticConstructObject(class,outer,FName(name),192)
    if not valid(object) then error("Cannot construct runtime media object: "..path) end
    return object
end
local objects
local function initializeMedia()
    local built={
        player=mediaObject("/Script/MediaAssets.MediaPlayer","TFVideoTuner_Player"),
        texture=mediaObject("/Script/MediaAssets.MediaTexture","TFVideoTuner_Texture"),
        sound=mediaObject("/Script/MediaAssets.MediaSoundComponent","TFVideoTuner_Sound"),
        target=mediaObject("/Script/Engine.TextureRenderTarget2D","TFVideoTuner_Output"),
    }
    -- Force the player factory proven to be in the packaged binary, rather than a default Bink player.
    built.player:SetDesiredPlayerName(FName("ElectraPlayer"))
    built.markers={}
    for i=1,native.stationCount() do
        -- Unregistered, mod-owned component keys for presentation only. They never
        -- become world radio sources or attach to the game's player/actor.
        built.markers[i]=mediaObject("/Script/MediaAssets.MediaSoundComponent","TFVideoTuner_Marker_"..i)
    end
    native.markers(built.markers)
    objects=built
    native.log("ready", "Standalone native video tuner ready; normal game tuning controls select stations")
end

local token={}
_G.__tf_video_token=token
_G.__tf_video_shutdown=function()
    screen.reset()
    native.close()
end
local perfAt,perfCalls,perfSeconds,perfMax=os.clock(),0,0,0
-- In the menus, and while the CRTV is down with nothing playing or fading, the game is read in full ten times a
-- second instead of every frame; between, only whether the CRTV came up (at once noticed). Playback runs every frame.
local IDLE_S=0.1
local idleUntil=-math.huge
LoopInGameThreadWithDelay(0,function()
    if _G.__tf_video_token~=token then return true end
    if os.clock()<idleUntil and not game.radioUp() then return false end
    local began=os.clock()
    local off
    local succeeded,message=pcall(function()
        local state=game.read()
        idleUntil=-math.huge
        if not objects then
            -- The media objects come once the DLL has a game profile (after a game update, a few seconds after start).
            local ready=native.ready()
            if ready==nil then off=true; return end
            if not (ready and state.gameplay) then idleUntil=os.clock()+IDLE_S; return end
            initializeMedia()
        end
        local currentScreen
        if state.active and not state.priority then currentScreen=screen.inspect(state.radio,state.frequency,objects.target) end
        if state.active and not state.priority and not currentScreen then state.priority=true end
        if state.priority or not state.active then screen.restore() end
        if currentScreen then
            state.width,state.height=currentScreen.width,currentScreen.height
            state.original=currentScreen.original
        end
        local out=native.update(state,objects,screen.isBound())
        if out and out.bind and not state.priority and state.active then screen.bind(currentScreen)
        else screen.restore() end
        if not state.active and out and out.phase==0 then idleUntil=os.clock()+IDLE_S end -- idle: down, nothing playing
    end)
    if not succeeded then
        -- Keep the loop alive after an invalidation/reflection failure. Restore before stopping native media.
        pcall(screen.restore)
        if objects then native.stop() end
        native.log("tick error", "Playback yielded: "..tostring(message))
    end
    if off then native.close(); return true end -- nothing to do this game run: the loop ends
    local elapsed=os.clock()-began
    perfCalls=perfCalls+1; perfSeconds=perfSeconds+elapsed; perfMax=math.max(perfMax,elapsed)
    if os.clock()-perfAt>=30 then
        print(string.format("[TF-VIDEO] Game thread: %.2f ms/s, %d ticks, max %.2f ms\n",
            perfSeconds*1000/(os.clock()-perfAt),perfCalls,perfMax*1000))
        perfAt,perfCalls,perfSeconds,perfMax=os.clock(),0,0,0
    end
    return false
end)
native.log("loading", "Waiting for game-thread media setup")
