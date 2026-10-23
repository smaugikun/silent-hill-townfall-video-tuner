"""Boundary tests; the native DLL/decoder is mocked here. Native logic has its own C tests."""
import shutil
import tempfile
import struct
import unittest
from pathlib import Path
from lupa import lua54
ROOT=Path(__file__).resolve().parents[1]
SCRIPTS=ROOT/'TownfallVideoTuner/Scripts'

class LuaBoundaryTests(unittest.TestCase):
    unsupported=False
    waitInfos=0
    def setUp(self):
        (ROOT/'build').mkdir(exist_ok=True)
        self.tmp=tempfile.TemporaryDirectory(dir=ROOT/'build')
        self.addCleanup(self.tmp.cleanup)
        stage=Path(self.tmp.name)/'Scripts'
        stage.mkdir()
        for source in SCRIPTS.glob('*.lua'): shutil.copy2(source,stage/source.name)
        self.lua=lua54.LuaRuntime()
        self.world=self.lua.execute((ROOT/'tests/fake_game.lua').read_text())(stage.as_posix(),
            self.lua.table(unsupported=self.unsupported,waitInfos=self.waitInfos))
        self.addCleanup(self.world.close)
        self.world.start()
    def tearDown(self):
        self.assertEqual(self.world.invalidCalls,0,'a freed UObject was accessed')
        self.assertEqual(self.world.controls,0,'normal game controls were modified')
    def tick(self,dt=.016): self.world.tick(dt)
    def bound(self): return self.world.material.texture.address==self.world.packet.target
    def test_binding_and_packet(self):
        self.tick()
        self.assertTrue(self.bound())
        self.assertEqual(self.world.packet.flags&8,8)
        self.assertEqual(self.world.packet.width,640)
        self.assertEqual(self.world.constructed,6)
        self.assertEqual(self.world.markerCount,2)
        self.assertEqual(self.world.desiredPlayer,'ElectraPlayer')
    def test_game_ranges_are_published_before_native_tick_and_while_lowered(self):
        self.world.pawn.active=False
        self.world.addSource(.9,.05)
        self.tick(.3)
        data=bytes.fromhex(self.world.rangeHex)
        magic,epoch,count,ready,revision,waypoint=struct.unpack_from('<8s4Id',data)
        low,high=struct.unpack_from('<2d',data,32)
        self.assertEqual((magic,count,ready),(b'TFVRANG2',1,1))
        self.assertAlmostEqual(waypoint,.05)  # the stations' range: the game's waypoints'
        self.assertAlmostEqual(low,.85)
        self.assertAlmostEqual(high,.95)
        self.assertEqual(epoch,self.world.packet.epoch)
        self.tick(.3)
        self.assertEqual(bytes.fromhex(self.world.rangeHex),data)
        self.assertEqual(self.world.rangeUpdates,1)
    def test_late_enemy_subclass_reserves_its_frequency(self):
        self.tick()
        self.world.addEnemySource(.15,.025)
        self.tick(.3)
        self.assertTrue(self.world.packet.flags&2)
        self.assertFalse(self.bound())
        data=bytes.fromhex(self.world.rangeHex)
        self.assertEqual(struct.unpack_from('<I',data,12)[0],1)
    def test_startup_with_strict_ue4ss_object_names(self):
        self.tick()
        self.assertEqual(self.world.constructed,6)
        self.assertEqual(self.world.inputSize,96)
        self.assertTrue(any('video tuner ready' in line for line in self.world.logs.values()))
    def test_media_construction_waits_for_game_thread(self):
        self.assertEqual(self.world.constructed,0)
        self.tick()
        self.assertEqual(self.world.constructed,6)
    def test_menu_skips_media_construction_and_world_scans(self):
        self.world.pawn['class']='DefaultPawn'
        for _ in range(10): self.tick()
        self.assertEqual(self.world.constructed,0)
        self.assertEqual(self.world.findAll,0)
        self.world.newLevel()
        self.tick()
        self.assertTrue(self.bound())
        self.assertEqual(self.world.constructed,6)
    def test_with_the_crtv_down_and_nothing_playing_the_game_is_read_ten_times_a_second(self):
        self.tick()
        self.world.pawn.active=False
        for _ in range(10): self.tick()  # the lowering is carried out, the player stops
        before=self.world.nativeTicks
        for _ in range(60): self.tick()  # about a second of frames
        self.assertLessEqual(self.world.nativeTicks-before,11)
        self.world.pawn.active=True
        before=self.world.nativeTicks
        self.tick()  # raised: noticed on the next frame, not after the idle wait
        self.assertEqual(self.world.nativeTicks,before+1)
        self.assertTrue(self.bound())
    def test_ticks_open_no_files(self):
        self.tick()
        opened=self.world.regularOpens
        for _ in range(300): self.tick()
        self.assertEqual(self.world.regularOpens,opened)
    def test_media_setup_retries_after_package_becomes_available(self):
        self.world.mediaPackage.destroyed=True
        self.tick()
        self.assertEqual(self.world.constructed,0)
        self.world.mediaPackage.destroyed=False
        self.tick()
        self.assertEqual(self.world.constructed,6)
        self.assertTrue(self.bound())
    def test_tuning_away_restores(self):
        self.tick(); self.world.nativeBind=False; self.world.pawn.frequency=.45; self.tick()
        self.assertEqual(self.world.material.texture.address,self.world.original.address)
    def test_crtv_close_restores_and_stops(self):
        self.tick(); self.world.pawn.active=False; self.tick()
        self.assertEqual(self.world.material.texture.address,self.world.original.address)
        self.assertFalse(self.world.packet.flags&1)
    def test_game_signal_has_priority(self):
        self.tick(); self.world.radio.cachedHighestSignalStrength=.1; self.tick()
        self.assertEqual(self.world.material.texture.address,self.world.original.address)
    def test_fine_tuning_and_story_yield(self):
        self.tick(); self.world.radio.fine=True; self.tick(); self.assertFalse(self.bound())
        self.world.radio.fine=False; self.world.radio.story=True; self.tick(); self.assertFalse(self.bound())
    def test_reserves_loaded_game_frequencies(self):
        self.world.addSource(.15,.02); self.tick(.3)
        self.assertEqual(self.world.packet.flags&2,2)
        self.assertEqual(self.world.material.texture.address,self.world.original.address)
    def test_blueprint_templates_do_not_reserve_channels(self):
        source=self.world.addSource(.15,1.0); source.owner=None
        self.tick(.3)
        self.assertFalse(self.world.packet.flags&2)
        self.assertTrue(self.bound())
    def test_cutscene_restores(self):
        self.tick(); sequence=self.world.addSequence(); sequence.playing=True; self.tick()
        self.assertEqual(self.world.packet.flags&2,2); self.assertFalse(self.bound())
    def test_pause_resume(self):
        self.tick(); self.world.paused=True; self.tick(); self.assertTrue(self.world.packet.flags&4)
        self.assertTrue(self.bound())
        self.world.paused=False; self.tick(); self.assertFalse(self.world.packet.flags&4)
    def test_level_transition_drops_invalid_objects(self):
        self.tick(); epoch=self.world.packet.epoch
        self.world.newLevel(); self.world.pawn.active=False; self.tick()
        self.assertGreater(self.world.packet.epoch,epoch)
        self.world.pawn.active=True; self.tick(); self.assertTrue(self.bound())
        self.assertEqual(self.world.constructed,6)
    def test_no_repeated_object_searches(self):
        self.tick(); searches=(self.world.findStatic,self.world.findAll)
        for _ in range(120): self.tick()
        self.assertEqual((self.world.findStatic,self.world.findAll),searches)
    def test_game_material_change_is_preserved(self):
        self.tick(); self.world.material.texture=self.world.original; self.tick()
        self.assertFalse(self.bound()); self.assertEqual(self.world.packet.flags&2,2)
        self.tick(); self.assertFalse(self.bound())
        self.world.pawn.active=False; self.tick(); self.world.pawn.active=True; self.tick(); self.assertTrue(self.bound())
    def test_decoder_error_restores(self):
        self.tick(); self.world.mediaError=True; self.tick(); self.assertFalse(self.bound())
        self.assertEqual(self.world.packet.flags&256,256)
    def test_destroyed_material_is_never_called(self):
        self.tick(); self.world.material.destroyed=True; self.tick()
        self.assertTrue(self.world.packet.flags&2)
    def test_destroyed_original_is_never_called(self):
        self.tick(); self.world.original.destroyed=True; self.world.pawn.active=False; self.tick()
        self.assertFalse(self.world.packet.flags&1)
    def test_invalid_source_data_stays_blocked_between_refreshes(self):
        source=self.world.addSource(.8,.01); source.TuningFrequency='unreadable'
        self.tick(.3); self.assertTrue(self.world.packet.flags&2)
        self.tick(.01); self.assertTrue(self.world.packet.flags&2)
    def test_dead_player_stops_playback(self):
        self.tick(); self.world.pawn.dead=True; self.tick(); self.assertFalse(self.world.packet.flags&1)
    def test_reload_reuses_objects_and_restores_texture(self):
        self.tick(); self.world.start(); self.tick()
        self.assertEqual(self.world.constructed,6)
        self.assertTrue(self.bound())
    def test_lower_and_raise_preserve_normal_game_dial_behavior(self):
        self.tick(); self.world.pawn.active=False; self.world.pawn.frequency=0; self.tick()
        self.world.pawn.active=True; self.tick()
        self.assertEqual(self.world.pawn.frequency,0)
        self.assertFalse(self.bound())
        self.world.pawn.frequency=.15; self.tick()
        self.assertTrue(self.bound())

class UnsupportedBuildTest(unittest.TestCase):
    unsupported=True
    waitInfos=0
    setUp=LuaBoundaryTests.setUp
    tearDown=LuaBoundaryTests.tearDown
    tick=LuaBoundaryTests.tick
    def test_disabled_before_constructing(self):
        self.tick(); self.assertEqual(self.world.constructed,0)
        self.assertTrue(any('disabled' in line for line in self.world.logs.values()))
        for _ in range(5): self.tick(1.1)
        self.assertEqual(self.world.infos,1)          # off for this game run: the loop ended, nothing more asked
        self.assertEqual(self.world.nativeTicks or 0,0)

class WaitingForProfileTest(unittest.TestCase):
    """After a game update the DLL's helper reads the game's symbols first: the mod waits, then starts as usual."""
    unsupported=False
    waitInfos=2
    setUp=LuaBoundaryTests.setUp
    tearDown=LuaBoundaryTests.tearDown
    tick=LuaBoundaryTests.tick
    bound=LuaBoundaryTests.bound
    def test_stations_come_on_once_the_profile_is_there(self):
        self.tick(); self.tick(.5)
        self.assertEqual((self.world.infos,self.world.constructed,self.world.nativeTicks or 0),(1,0,0))  # once a second
        self.assertTrue(any("Reading the game's symbols" in line for line in self.world.logs.values()))
        self.tick(.6); self.assertEqual((self.world.infos,self.world.constructed),(2,0))
        self.tick(1.1); self.assertEqual(self.world.infos,3)
        self.assertEqual(self.world.constructed,6)   # the four media objects and the two stations' markers
        self.assertEqual(self.world.markerCount,2)
        self.tick()
        self.assertTrue(self.bound())

if __name__=='__main__': unittest.main()
