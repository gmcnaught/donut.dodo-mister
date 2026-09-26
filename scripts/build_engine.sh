#!/bin/sh
# Build the Donut Dodo engine: Godot 3.5.2-stable + FRT 2.1.0 (frt@33f739d in platform/frt,
# patches 0002 and 0006 applied), armhf Cortex-A9, pruned to what the game uses (PLAN.md §1l):
#   no 3D; modules kept: gdscript (.gdc scripts), freetype (DynamicFont), stb_vorbis (.oggstr music),
#   webp (every .stex is WebP), navigation (3.5's Navigation2DServer needs its NavigationServer;
#   without it the engine segfaults at start). disable_advanced_gui stays off: Options.tscn uses
#   OptionButton, and it would also remove MarginContainer/ViewportContainer.
#   scripts/build_engine.sh [godot tree, default work/godot-3.5.2-prune]
# Output: <tree>/bin/godot.frt.opt.arm32v7 (make_release.sh strips it). Needs >= 8 GiB for the
# Docker VM at -j6 (2 GiB OOM-kills cc1plus).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TREE=${1:-work/godot-3.5.2-prune}
docker image inspect frt-armhf-build:bullseye >/dev/null 2>&1 \
	|| docker build -t frt-armhf-build:bullseye -f "$ROOT/Dockerfile.frt-build" "$ROOT"
# The image's pkg-config points at /src/sdl2-armhf (headers + libSDL2.so to link against).
docker run --rm -v "$ROOT/work":/src -v "$ROOT/$TREE":/godot -w /godot frt-armhf-build:bullseye \
	scons -j${JOBS:-6} platform=frt frt_arch=arm32v7 frt_cross=auto target=release tools=no \
	disable_3d=yes \
		module_bmp_enabled=no module_bullet_enabled=no module_camera_enabled=no module_csg_enabled=no  \
	module_cvtt_enabled=no module_dds_enabled=no module_denoise_enabled=no module_enet_enabled=no  \
	module_etc_enabled=no module_fbx_enabled=no module_gdnative_enabled=no module_gltf_enabled=no  \
	module_gridmap_enabled=no module_hdr_enabled=no module_jpg_enabled=no module_jsonrpc_enabled=no  \
	module_lightmapper_cpu_enabled=no module_mbedtls_enabled=no module_minimp3_enabled=no  \
	module_mobile_vr_enabled=no module_mono_enabled=no module_ogg_enabled=no  \
	module_opensimplex_enabled=no module_opus_enabled=no module_pvr_enabled=no  \
	module_raycast_enabled=no module_regex_enabled=no module_squish_enabled=no module_svg_enabled=no  \
	module_tga_enabled=no module_theora_enabled=no module_tinyexr_enabled=no module_upnp_enabled=no  \
	module_vhacd_enabled=no module_visual_script_enabled=no module_vorbis_enabled=no  \
	module_webm_enabled=no module_webrtc_enabled=no module_websocket_enabled=no module_webxr_enabled=no  \
	module_xatlas_unwrap_enabled=no 
