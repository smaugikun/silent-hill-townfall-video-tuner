-- Test world for the Lua/game boundary. This never pretends to decode media.
return function(scripts, options)
    local root=scripts:gsub("/Scripts$", "")
    local world={logs={},invalidCalls=0,controls=0,findAll=0,findStatic=0,constructed=0,
                 paused=false,nativeBind=true,draws=0,stops=0,clock=0}
    local objects,instances,loops,notify={}, {}, {}, {}
    local serial=1000
    local function object(t)
        serial=serial+1; t.address=serial; objects[serial]=t
        t.IsValid=function(self) return not self.destroyed end
        t.GetAddress=function(self) if self.destroyed then world.invalidCalls=world.invalidCalls+1 end; return self.address end
        t.GetWorld=function(self) if self.destroyed then world.invalidCalls=world.invalidCalls+1 end; return world.gameWorld end
        t.IsA=function(self,class) return self.class==class.path end
        return t
    end
    local function method(t,name,fn)
        t[name]=function(self,...)
            if self.destroyed then world.invalidCalls=world.invalidCalls+1; error("freed object") end
            return fn(self,...)
        end
    end
    local classes={}
    for _,path in ipairs({"/Script/Townfall.TownfallPlayerCharacter",
        "/Script/MediaAssets.MediaPlayer","/Script/MediaAssets.MediaTexture","/Script/MediaAssets.MediaSoundComponent",
        "/Script/Engine.TextureRenderTarget2D","/Script/LevelSequence.LevelSequencePlayer",
        "/Script/Townfall.RadioWaypointSourceComponent","/Script/Townfall.RadioStaticSourceComponent",
        "/Script/Townfall.TownfallEnemyCharacter"}) do
        classes[path]=object({path=path})
    end
    world.mediaPackage=object({path="/Script/MediaAssets"})
    method(world.mediaPackage,"GetFullName",function() return "Package /Script/MediaAssets" end)
    method(classes["/Script/MediaAssets.MediaPlayer"],"GetOuter",function() return world.mediaPackage end)
    local statics=object({})
    method(statics,"IsGamePaused",function() return world.paused end)
    classes["/Script/Engine.Default__GameplayStatics"]=statics
    function world.newLevel()
        for _,o in ipairs(world.levelObjects or {}) do o.destroyed=true end
        local level={}
        local function localObject(t) local o=object(t); level[#level+1]=o; return o end
        world.gameWorld=localObject({})
        world.original=localObject({SizeX=640,SizeY=512})
        world.material=localObject({texture=world.original})
        method(world.material,"K2_GetTextureParameterValue",function(self,param) assert(param=="RadarTexture"); return self.texture end)
        method(world.material,"SetTextureParameterValue",function(self,param,texture) assert(param=="RadarTexture"); self.texture=texture; world.setterCalls=(world.setterCalls or 0)+1 end)
        world.widget=localObject({})
        method(world.widget,"GetRenderTarget",function() return world.original end)
        world.radio=localObject({PlayerCharacterType=0,cachedHighestSignalStrength=0,fine=false,story=false})
        method(world.radio,"GetCRTVWidget",function() return world.widget end)
        local setup={RadarTexture={Widget=world.widget,Material=world.material,MaterialTextureName="RadarTexture"}}
        world.radio.RadarPropMap={Find=function(_,key) assert(key==0); return {get=function() return setup end} end}
        world.pawn=localObject({class="/Script/Townfall.TownfallPlayerCharacter",Health=100,active=true,frequency=.15,dead=false})
        method(world.pawn,"GetIsRadioInActiveMode",function(self) return self.active end)
        method(world.pawn,"GetRadio",function() return world.radio end)
        method(world.pawn,"GetRadioCurrentTunedFrequency",function(self) return self.frequency end)
        method(world.pawn,"IsDead",function(self) return self.dead end)
        for _,name in ipairs({"SetRadioInActiveMode","SetRadioCurrentTunedFrequency","SetHiddenInGame","SetTunedFrequency",
             "PlayerInput_FineTuneConfirm_Pressed","PlayerInput_FineTuneConfirm_Released"}) do
            method(world.pawn,name,function() world.controls=world.controls+1; error("game control mutation") end)
            method(world.radio,name,function() world.controls=world.controls+1; error("game control mutation") end)
        end
        world.levelObjects=level
        if world.controller then world.controller.Pawn=world.pawn end
        world.sources={}; world.sequences={}; world.enemies={}
    end
    world.newLevel()
    world.controller=object({Pawn=world.pawn})
    function world.addSource(f,t)
        local source=object({TuningFrequency=f,TuningTolerance_Outer=t})
        source.class="/Script/Townfall.RadioWaypointSourceComponent"
        source.owner=object({})
        method(source,"GetOwner",function(self) return self.owner end)
        world.sources[#world.sources+1]=source
        local callback=notify["/Script/Townfall.RadioWaypointSourceComponent"]
        if callback then callback(source) end
        return source
    end
    function world.addSequence()
        local seq=object({playing=false}); method(seq,"IsPlaying",function(self) return self.playing end)
        world.sequences[#world.sequences+1]=seq
        if notify["/Script/LevelSequence.LevelSequencePlayer"] then notify["/Script/LevelSequence.LevelSequencePlayer"](seq) end
        return seq
    end
    function world.addEnemySource(f,t)
        local source=object({TuningFrequency=f,TuningTolerance_Outer=t})
        source.owner=object({})
        method(source,"GetOwner",function(self) return self.owner end)
        local enemy=object({RadioStaticSource=source})
        world.enemies[#world.enemies+1]=enemy
        if notify["/Script/Townfall.TownfallEnemyCharacter"] then notify["/Script/Townfall.TownfallEnemyCharacter"](enemy) end
        return enemy,source
    end
    print=function(s) world.logs[#world.logs+1]=s end
    os.clock=function() return world.clock end
    package.path=scripts.."/?.lua;"..package.path
    package.preload.UEHelpers=function() return {GetPlayerController=function() return world.controller end} end
    StaticFindObject=function(path)
        world.findStatic=world.findStatic+1
        if not path:find(".",1,true) then error("GetPackageNameFromLongName: Name wasn't long.") end
        return classes[path] or instances[path]
    end
    FindAllOf=function(name)
        world.findAll=world.findAll+1
        if name=="Package" then return {} end -- observed during real game startup
        if name=="RadioWaypointSourceComponent" then return world.sources end
        if name=="TownfallEnemyCharacter" then return world.enemies end
        if name=="LevelSequencePlayer" then return world.sequences end
        return {}
    end
    NotifyOnNewObject=function(path,callback) notify[path]=callback end
    FName=function(name) return name end
    StaticConstructObject=function(class,outer,name,flags)
        assert(world.onGameThread and class:IsValid() and outer:IsValid() and outer==world.mediaPackage and flags==192)
        world.constructed=world.constructed+1
        local obj=object({class=class.path,flags=flags,SizeX=640,SizeY=512})
        if class.path=="/Script/MediaAssets.MediaPlayer" then
            method(obj,"HasError",function() return world.mediaError or false end)
            method(obj,"SetDesiredPlayerName",function(_,n) assert(n=="ElectraPlayer"); world.desiredPlayer=n end)
        end
        instances[world.mediaPackage.path.."."..name]=obj
        return obj
    end
    LoopAsync=function() error("async Lua is forbidden") end
    ExecuteInGameThread=function() error("cross-thread Lua is forbidden") end
    RegisterHook=function() error("game input hooks are forbidden") end
    LoopInGameThreadWithDelay=function(delay,fn) assert(delay==0); loops[#loops+1]=fn end
    local ordinaryOpen=io.open
    world.regularOpens=0
    io.open=function(path,mode)
        if path:find("\\\\.\\pipe\\TFVideoTuner-",1,true)==1 then
            assert(mode=="r+b")
            local input=path:sub(-4)~="-out"
            return {
                setvbuf=function(_,buffering) assert(buffering=="no"); return true end,
                write=function(self,bytes) assert(input); world.inputBytes=(world.inputBytes or "")..bytes; return self end,
                read=function(_,size)
                    assert(not input and #(world.outputBytes or "")>=size,"native reply was not ready")
                    local bytes=world.outputBytes:sub(1,size); world.outputBytes=world.outputBytes:sub(size+1); return bytes
                end,
                close=function() return true end,
            }
        end
        world.regularOpens=world.regularOpens+1
        return ordinaryOpen(path,mode)
    end
    local function reply(seq,phase,station,bind,flags,errorCode)
        world.outputBytes=(world.outputBytes or "")..string.pack("<c8I8I4i4I4I4I8i8I4I4","TFVSTAT1",seq,phase,station,bind and 1 or 0,options.stationCount or 2,world.draws,0,flags,errorCode or 0)
    end
    package.loadlib=function(_,symbol)
        if symbol=="tf_video_init" then return function()
            world.inputBytes,world.outputBytes="",""
            local f=assert(ordinaryOpen(root.."/video-bridge.ini","wb"))
            f:write("\\\\.\\pipe\\TFVideoTuner-1-2-3-in\n\\\\.\\pipe\\TFVideoTuner-1-2-3-in-out\n"); f:close()
        end end
        -- unsupported: off for this game (code 1); waitInfos: the first so many answers still read the game's symbols (7).
        if symbol=="tf_video_info" then return function()
            world.infos=(world.infos or 0)+1
            local waiting=world.infos<=(options.waitInfos or 0)
            reply(0,0,-1,false,(options.unsupported or waiting) and 0 or 257,options.unsupported and 1 or waiting and 7 or 0)
        end end
        if symbol=="tf_video_shutdown" then return function() world.stops=world.stops+1 end end
        if symbol=="tf_video_tick" then return function()
            world.nativeTicks=(world.nativeTicks or 0)+1
            local bytes
            while #world.inputBytes>0 do
                local magic,kind,size,pos=string.unpack("<c8I4I4",world.inputBytes)
                assert(magic=="TFVPIPE1" and size<=4120 and #world.inputBytes>=16+size)
                local payload=world.inputBytes:sub(pos,pos+size-1)
                world.inputBytes=world.inputBytes:sub(pos+size)
                if kind==1 then bytes=payload
                elseif kind==2 then world.markerBytes=payload
                elseif kind==3 then
                    world.rangeHex=payload:gsub(".",function(c) return string.format("%02x",c:byte()) end)
                    world.rangeUpdates=(world.rangeUpdates or 0)+1
                else error("bad bridge kind") end
            end
            local markerBytes=assert(world.markerBytes)
            assert(#markerBytes==528)
            local magicMarker,count=string.unpack("<c8I4I4",markerBytes)
            assert(magicMarker=="TFVMARK1" and count==(options.stationCount or 2))
            for i=1,count do
                local addr=string.unpack("<I8",markerBytes,17+(i-1)*8)
                assert(objects[addr] and objects[addr].class=="/Script/MediaAssets.MediaSoundComponent")
                assert(objects[addr].flags==192 and not objects[addr].GetOwner)
            end
            world.markerCount=count
            assert(#bytes==96)
            world.inputSize=#bytes
            local magic,seq,player,texture,sound,target,gameWorld,radio,frequency,flags,epoch,width,height,original=string.unpack("<c8I8I8I8I8I8I8I8dI4I4I4I4I8",bytes)
            assert(magic=="TFVIDEO1")
            world.packet={flags=flags,epoch=epoch,frequency=frequency,player=player,target=target,width=width,height=height,original=original}
            local nativePriority=world.radio.fine or world.radio.story or world.radio.cachedHighestSignalStrength>0
            local station=math.abs(frequency-.15)<=.015 and 0 or math.abs(frequency-.75)<=.015 and 1 or -1
            local bind=station>=0 and flags&1~=0 and flags&2==0 and not nativePriority and world.nativeBind and not world.mediaError
            local phase=bind and (flags&4~=0 and 3 or 2) or 0
            if bind and phase==2 then world.draws=world.draws+1 end
            if bind then assert(objects[player] and objects[texture] and objects[sound] and objects[target] and objects[gameWorld] and objects[radio]) end
            reply(seq,phase,bind and station or -1,bind,257|((nativePriority or flags&2~=0) and 4 or 0),world.mediaError and 5 or 0)
        end end
        return nil,"symbol missing"
    end
    function world.tick(dt)
        world.clock=world.clock+(dt or 0.016)
        world.onGameThread=true
        local keep={}
        for _,loop in ipairs(loops) do if not loop() then keep[#keep+1]=loop end end
        loops=keep
        world.onGameThread=false
    end
    function world.start() dofile(scripts.."/main.lua") end
    function world.close() if _G.__tf_video_shutdown then _G.__tf_video_shutdown() end end
    return world
end
