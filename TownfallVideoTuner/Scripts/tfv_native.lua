local M = {}
local here = debug.getinfo(1,"S").source:match("^@(.*[\\/])")
local root = here and here:gsub("[\\/]Scripts[\\/]$", "")
local init, info, tick, shutdown, packet, status
local sequence, lastLog, stationCount = 0, {}, 0
local rangeKey,rangeRevision

function M.log(channel, message)
    if lastLog[channel] ~= message then
        lastLog[channel] = message
        print("[TF-VIDEO] " .. message .. "\n")
    end
end
function M.valid(object)
    return object ~= nil and object.IsValid ~= nil and object:IsValid()
end
local function address(object)
    return M.valid(object) and object:GetAddress() or 0
end
local function readStatus()
    if not status then return nil end
    local bytes = status:read(56)
    if not bytes or #bytes ~= 56 then return nil end
    local magic, seq, phase, station, bind, count, draws, position, flags, errorCode =
        string.unpack("<c8I8I4i4I4I4I8i8I4I4", bytes)
    if magic ~= "TFVSTAT1" then return nil end
    return {seq=seq,phase=phase,station=station,bind=bind~=0,draws=draws,position=position,
            supported=flags&1~=0,audio=(flags>>8)&3,error=errorCode,stations=count}
end
local function send(kind,bytes)
    assert(packet:write(string.pack("<c8I4I4","TFVPIPE1",kind,#bytes)..bytes))
end
function M.init()
    if not root then return false end
    local dll = here .. "tf_video.dll"
    local err
    init, err = package.loadlib(dll, "tf_video_init")
    info = package.loadlib(dll, "tf_video_info")
    tick = package.loadlib(dll, "tf_video_tick")
    shutdown = package.loadlib(dll, "tf_video_shutdown")
    if not (init and info and tick and shutdown) then M.log("dll", "DLL unavailable: " .. tostring(err)); return false end
    init()
    local descriptor=io.open(root.."/video-bridge.ini","rb")
    if not descriptor then M.log("dll","Local memory bridge unavailable; see tf-video.log"); return false end
    local inputName,outputName=descriptor:read("l"),descriptor:read("l"); descriptor:close()
    if not (inputName and outputName and inputName:match("^\\\\%.\\pipe\\TFVideoTuner%-%d+%-%d+%-%d+%-in$")
        and outputName==inputName.."-out") then M.log("dll","Invalid local memory bridge descriptor"); return false end
    -- r+b opens an existing pipe. Each unbuffered handle uses one direction only.
    packet=io.open(inputName,"r+b"); status=io.open(outputName,"r+b")
    if not (packet and status) then M.log("dll","Cannot connect local memory bridge"); M.close(); return false end
    assert(packet:setvbuf("no")); assert(status:setvbuf("no"))
    return true
end
-- Whether the DLL makes engine calls: once it has a game profile that fits this game. After a game update its helper
-- reads the game's symbols first (code 7, a few seconds). Asked at most once a second until then. nil: not this game
-- run (code 1: no profile for this game version; 2: stations.ini is wrong).
local ready, askedAt = false, -math.huge
function M.ready()
    if ready or os.clock() - askedAt < 1 then return ready end
    askedAt = os.clock()
    info()
    local state = readStatus()
    if not (state and state.supported) then
        local waiting = state and state.error == 7
        M.log("dll", waiting and "Reading the game's symbols for this game version; the stations come on after that"
            or "Native mod disabled (code " .. tostring(state and state.error) .. "); see tf-video.log")
        if waiting then return false end
        return nil
    end
    stationCount=math.min(state.stations or 0,64)
    -- A Lua reload releases decoder/audio activity before reusing the same four rooted media objects.
    shutdown()
    ready = true
    return true
end
function M.stationCount() return stationCount end
function M.markers(sources)
    -- One bounded setup packet, rather than rewriting all source pointers every frame.
    local parts={string.pack("<c8I4I4","TFVMARK1",stationCount,0)}
    for i=1,64 do parts[#parts+1]=string.pack("<I8",address(sources[i])) end
    send(2,table.concat(parts))
end
function M.update(state, objects, bound)
    local ranges=state.ranges or {}
    local ready=state.reservationValid and #ranges<=256
    local parts={string.pack("<I4I4I4",state.epoch or 0,ready and #ranges or 0,ready and 1 or 0)}
    if ready then
        for _,range in ipairs(ranges) do
            parts[#parts+1]=string.pack("<dd",math.max(0,range[1]),math.min(1,range[2]))
        end
    end
    local key=table.concat(parts)
    if rangeKey~=key then
        rangeRevision=(rangeRevision or 0)+1
        send(3,"TFVRANG1"..key:sub(1,12)..string.pack("<I4",rangeRevision)..key:sub(13))
        rangeKey=key
    end
    sequence = sequence + 1
    local flags = (state.active and 1 or 0) | (state.priority and 2 or 0) | (state.paused and 4 or 0)
    if M.valid(objects.player) and M.valid(objects.texture) and M.valid(objects.sound) and M.valid(objects.target) then
        flags = flags | 8
        local ok, failed = pcall(function() return objects.player:HasError() end)
        if ok and failed then flags = flags | 256 end
    end
    if bound then flags = flags | 16 end
    send(1,string.pack("<c8I8I8I8I8I8I8I8dI4I4I4I4I8", "TFVIDEO1", sequence,
        address(objects.player),address(objects.texture),address(objects.sound),address(objects.target),
        address(state.world),address(state.radio),state.frequency or 0,flags,state.epoch or 0,state.width or 640,state.height or 512,address(state.original)))
    tick()
    local out = readStatus()
    if not out or out.seq ~= sequence then return nil end
    local phases = {[0]="idle","opening","playing","paused","fading","ended","error"}
    M.log("phase", string.format("%s station=%d nativeDraws=%d%s",phases[out.phase] or tostring(out.phase),out.station,
        -- Do not log a changing frame counter every game tick.
        out.phase==2 and 0 or out.draws,out.error~=0 and (" error="..out.error) or ""))
    local routes = {[0]="waiting for audio", "FMOD CRTV_Videos bus", "Windows PCM output", "audio unavailable"}
    M.log("audio", "Audio: " .. routes[out.audio])
    return out
end
function M.stop()
    if shutdown then shutdown() end
end
function M.close()
    M.stop()
    if packet then packet:close(); packet=nil end
    if status then status:close(); status=nil end
    rangeKey,rangeRevision=nil,nil
end
return M
