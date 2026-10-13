# Port-owned developer cheats for testing Donut Dodo on MiSTer (not game content).
#
# Off unless games/DonutDodo/dev.cfg exists (or MISTER_DEV=1): mister_layout.gd
# adds this node only then. It never edits the game's files.
#
# Pad controls, on the shoulder buttons the game leaves unbound (Godot raw
# buttons 4 = L, 5 = R). Select is not used: in a level it pauses the game.
#   R (tap)     in a level: warp to the next level
#               anywhere else (menus, attract, game over): start a game at start_level
#   L (tap)     in a level: warp to the previous level
#   L + R       toggle invincibility; a tap that becomes part of the chord warps nowhere
#
# dev.cfg, "key = value" lines, # comments, every key optional:
#   invincible  = 1          start invincible (0: normal hits)
#   start_level = 1          where R starts a game: 1-10, or "bonus"
#   mode        = easy_mode  game mode R starts: easy_mode, medium_mode, speedrun_mode
#
# Level numbers are the game's own (GameManager.Level, which sets difficulty and
# the last level): 1-5 are maps 1-5, the bonus level runs as level 6, and 6-10
# replay maps 1-5. Invincibility disconnects the player's HurtBox "hit" signal,
# the game's only path to hurt_player(); timers running out never kill.
extends Node

const L_BUTTON := 4
const R_BUTTON := 5
const LEVELS := "res://Scenes/Levels/"
# [GameManager.Level, scene] in the order the game plays them (Level.gd load_next_level).
const STOPS := [
	[1, "Level_01"], [2, "Level_02"], [3, "Level_03"], [4, "Level_04"], [5, "Level_05"],
	[6, "Level_Bonus"],
	[6, "Level_01"], [7, "Level_02"], [8, "Level_03"], [9, "Level_04"], [10, "Level_05"],
]
const BONUS_STOP := 5

var cfg_path := ""
var invincible := true
var start_stop := 0
var mode := "easy_mode"
var held := {}
var chord := false   # L and R were down together since both were last up
var label: Label
var toast_left := 0.0


func _init(path := ""):
	cfg_path = path


func _ready():
	_read_cfg()
	var layer := CanvasLayer.new()
	layer.layer = 128
	add_child(layer)
	label = Label.new()
	var font = load("res://Fonts/DefaultFont.tres")
	if font:
		label.add_font_override("font", font)
	label.rect_position = Vector2(8, 200)
	label.visible = false
	layer.add_child(label)
	printerr("mister_dev: on (invincible=%s start=%s mode=%s)" % [str(invincible), _stop_name(start_stop), mode])


func _read_cfg():
	var f := File.new()
	if cfg_path == "" or not f.file_exists(cfg_path) or f.open(cfg_path, File.READ) != OK:
		return
	while not f.eof_reached():
		var kv: PoolStringArray = f.get_line().split("#")[0].split("=", true, 1)
		if kv.size() != 2:
			continue
		var key: String = kv[0].strip_edges()
		var v: String = kv[1].strip_edges()
		match key:
			"invincible":
				invincible = v != "0"
			"start_level":
				if v == "bonus":
					start_stop = BONUS_STOP
				elif v.is_valid_integer() and int(v) >= 1 and int(v) <= 10:
					# 6 is map 1 after the bonus level, as in a real game
					start_stop = int(v) - 1 if int(v) <= 5 else int(v)
				else:
					printerr("mister_dev: start_level \"%s\" ignored (want 1-10 or bonus)" % v)
			"mode":
				if v in ["easy_mode", "medium_mode", "speedrun_mode"]:
					mode = v
				else:
					printerr("mister_dev: mode \"%s\" ignored" % v)
	f.close()


func _process(delta):
	if toast_left > 0.0:
		toast_left -= delta
		if toast_left <= 0.0:
			label.visible = false
	var gm = get_node_or_null("/root/GameManager")
	if gm == null:
		return
	_apply_invincibility(gm)

	var l := _pressed(L_BUTTON)
	var r := _pressed(R_BUTTON)
	if l and r and not chord:
		chord = true
		invincible = not invincible
		_toast("INVINCIBLE " + ("ON" if invincible else "OFF"))
	# Warps act on release, so the first button of a chord does nothing.
	if not chord:
		if held.get(L_BUTTON, false) and not l and _in_level(gm):
			call_deferred("_warp", _current_stop(gm) - 1)
		if held.get(R_BUTTON, false) and not r:
			if _in_level(gm):
				call_deferred("_warp", _current_stop(gm) + 1)
			else:
				call_deferred("_new_game")
	elif not l and not r:
		chord = false
	held[L_BUTTON] = l
	held[R_BUTTON] = r


func _pressed(button: int) -> bool:
	for dev in Input.get_connected_joypads():
		if Input.is_joy_button_pressed(dev, button):
			return true
	return false


func _apply_invincibility(gm):
	var p = gm.Player
	if p == null or not is_instance_valid(p) or not p.has_node("HurtBox"):
		return
	var hurtbox = p.get_node("HurtBox")
	if not hurtbox.is_connected("hit", self, "_on_hit"):
		hurtbox.connect("hit", self, "_on_hit")
		p.connect("player_hurt", self, "_on_player_hurt")
	var connected: bool = hurtbox.is_connected("hit", p, "_on_HurtBox_hit")
	if invincible and connected:
		hurtbox.disconnect("hit", p, "_on_HurtBox_hit")
	elif not invincible and not connected:
		hurtbox.connect("hit", p, "_on_HurtBox_hit")


func _on_hit(_damage):
	if invincible:
		printerr("mister_dev: hit ignored")


func _on_player_hurt():
	printerr("mister_dev: player hurt")


func _scene_manager():
	return get_node_or_null("/root/Main/SceneManager")


func _in_level(gm) -> bool:
	var p = gm.Player
	return p != null and is_instance_valid(p) and p.is_inside_tree() and _scene_manager() != null


func _stops(gm) -> int:
	# Modes stop at their own last level: speedrun has 5 and no bonus level.
	var n := 0
	for s in STOPS:
		if s[0] <= gm.current_game_mode.Levels:
			n += 1
	return n


func _current_stop(gm) -> int:
	var sm = _scene_manager()
	var scene: String = sm.get_child(0).filename if sm.get_child_count() > 0 else ""
	for i in range(STOPS.size()):
		if STOPS[i][0] == gm.Level and scene == LEVELS + STOPS[i][1] + ".tscn":
			return i
	for i in range(STOPS.size()):
		if STOPS[i][0] == gm.Level:
			return i
	return 0


func _warp(stop: int):
	var gm = get_node_or_null("/root/GameManager")
	var sm = _scene_manager()
	if gm == null or sm == null or get_tree().paused:
		return
	stop = int(clamp(stop, 0, _stops(gm) - 1))
	var level: int = STOPS[stop][0]
	gm.Level = level
	gm.game_won = false
	# The game raises difficulty once, when it reaches IncreaseDifficultyLevel.
	var m = gm.current_game_mode
	gm.Difficulty = m.SecondDifficulty if level >= m.IncreaseDifficultyLevel else m.FirstDifficulty
	var sounds = get_node_or_null("/root/Sounds")
	if sounds:
		sounds.stop_all_sounds()
	_toast(_stop_name(stop))
	sm.load_scene(LEVELS + STOPS[stop][1] + ".tscn")


func _new_game():
	var gm = get_node_or_null("/root/GameManager")
	if gm == null or _scene_manager() == null or get_tree().paused:
		return
	gm.set_game_mode(mode)
	gm.initialize_new_game()
	_warp(start_stop)


func _stop_name(stop: int) -> String:
	if stop == BONUS_STOP:
		return "LEVEL 6 BONUS"
	return "LEVEL %d" % STOPS[stop][0]


func _toast(text: String):
	printerr("mister_dev: " + text)
	label.text = text
	label.visible = true
	toast_left = 2.5
