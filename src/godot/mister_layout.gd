# Port-owned layout tweaks for Donut Dodo on a CRT (not game content).
#
# Registered by the engine as an extra autoload through MISTER_GODOT_AUTOLOAD
# (patches/0008), after the game's own autoloads. It never edits the game's
# files: it moves nodes the game creates, once each, after their _ready().
#
# hud_offset = N  (display.cfg, MISTER_GLUE_SAFE_AREA_FILE; else MISTER_HUD_OFFSET)
#   Moves the HUD (Scenes/UI/HUD.tscn, in every level and the attract screen)
#   down N rows, all but its black bar: with N = 8 the labels sit on screen rows
#   15-21 and the values on 23-29, clear of a CRT's top overscan. HUD.gd _ready()
#   places the HUD at global (0, 6) and nothing moves it afterwards.
#   0 (default) = the game's own layout.
extends Node

const KEY := "hud_offset"
const MIN_OFFSET := -16
const MAX_OFFSET := 32

var hud_offset := 0


func _ready():
	hud_offset = _read_offset()
	print("mister_layout: hud_offset=%d" % hud_offset)
	if hud_offset != 0:
		get_tree().connect("node_added", self, "_on_node_added")


func _read_offset() -> int:
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


func _on_node_added(node: Node):
	# The HUD scene's root: a Control named HUD with the game's HUD children.
	if node.name == "HUD" and node is Control:
		node.connect("ready", self, "_on_hud_ready", [node], CONNECT_ONESHOT)


func _on_hud_ready(hud: Control):
	if not hud.has_node("HighScoreLabel") or not hud.has_node("ScoreText"):
		return
	hud.rect_position.y += hud_offset
	var bar := hud.get_node_or_null("ColorRect") as Control
	if bar:
		bar.rect_position.y -= hud_offset   # the black bar stays where the game put it
