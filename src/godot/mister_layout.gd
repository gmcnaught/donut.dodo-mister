# Port-owned layout tweaks for Donut Dodo on a CRT (not game content).
#
# Registered by the engine as an extra autoload through MISTER_GODOT_AUTOLOAD
# (patches/0008), after the game's own autoloads. It never edits the game's
# files: it moves nodes the game creates, after their _ready().
#
# HUD offset: moves the HUD (Scenes/UI/HUD.tscn, in every level and the attract
# screen) down N rows, all but its black bar: with N = 8 the labels sit on
# screen rows 15-21 and the values on 23-29, clear of a CRT's top overscan.
# HUD.gd _ready() places the HUD at global (0, 6) and nothing moves it after.
# N comes from, first match wins:
#   OSD HUD Down (non-zero), published live by the glue as MISTER_HUD_OFFSET_OSD
#   hud_offset = N in display.cfg (MISTER_GLUE_SAFE_AREA_FILE)
#   MISTER_HUD_OFFSET
#   0: the game's own layout.
extends Node

const KEY := "hud_offset"
const MIN_OFFSET := -16
const MAX_OFFSET := 32
const APPLIED := "mister_hud_offset"   # meta: rows this script has moved a HUD by

var cfg_offset := 0
var huds := []


func _ready():
	cfg_offset = _read_cfg_offset()
	print("mister_layout: hud_offset=%d" % cfg_offset)
	get_tree().connect("node_added", self, "_on_node_added")


func _read_cfg_offset() -> int:
	var v := OS.get_environment("MISTER_HUD_OFFSET")
	var path := OS.get_environment("MISTER_GLUE_SAFE_AREA_FILE")
	var f := File.new()
	if path != "" and f.file_exists(path) and f.open(path, File.READ) == OK:
		while not f.eof_reached():
			var line: String = f.get_line().split("#")[0]
			var kv := line.split("=", true, 1)
			if kv.size() == 2 and kv[0].strip_edges() == KEY:
				v = kv[1].strip_edges()
				break
		f.close()
	if not v.is_valid_integer():
		if v != "":
			print("mister_layout: %s \"%s\" ignored (want an integer)" % [KEY, v])
		return 0
	return int(clamp(int(v), MIN_OFFSET, MAX_OFFSET))


# The rows the HUD should be moved by right now: a non-zero OSD step wins.
func _target() -> int:
	var osd := OS.get_environment("MISTER_HUD_OFFSET_OSD")
	if osd.is_valid_integer() and int(osd) != 0:
		return int(clamp(int(osd), MIN_OFFSET, MAX_OFFSET))
	return cfg_offset


func _on_node_added(node: Node):
	# The HUD scene's root: a Control named HUD with the game's HUD children.
	if node.name == "HUD" and node is Control:
		node.connect("ready", self, "_on_hud_ready", [node], CONNECT_ONESHOT)


func _on_hud_ready(hud: Control):
	if not hud.has_node("HighScoreLabel") or not hud.has_node("ScoreText"):
		return
	hud.set_meta(APPLIED, 0)
	huds.append(hud)
	_apply(hud, _target())


func _process(_delta):
	var target := _target()
	for i in range(huds.size() - 1, -1, -1):
		var hud = huds[i]
		if not is_instance_valid(hud):
			huds.remove(i)
		elif hud.get_meta(APPLIED) != target:
			_apply(hud, target)


func _apply(hud: Control, target: int):
	var delta: int = target - int(hud.get_meta(APPLIED))
	if delta == 0:
		return
	hud.rect_position.y += delta
	var bar := hud.get_node_or_null("ColorRect") as Control
	if bar:
		bar.rect_position.y -= delta   # the black bar stays where the game put it
	hud.set_meta(APPLIED, target)
