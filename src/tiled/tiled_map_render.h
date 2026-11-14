#ifndef _TILED_MAP_RENDER_H_
#define _TILED_MAP_RENDER_H_

#include "renderer/renderer.h"
#include "tiled/tiled_map.h"

#ifdef __cplusplus
extern "C" {
#endif

// Extra tiles drawn beyond the exact camera viewport, on every side, so panning never pops a
// tile in right at the screen edge : the world already visibly extends a bit past the frame.
#define TILED_MAP_CULLING_MARGIN 1

// Called once per visible, non-empty tile. `dest_*` is the tile's on-screen rectangle (already
// offset by the camera, in whatever pixel space the caller's `viewport_width`/`viewport_height`
// was expressed in) ; `src_*` is its source rectangle within the tileset texture. The caller
// decides how to actually draw it (e.g. straight to the sprite batch, or further scaled/offset
// for a pixel-perfect virtual canvas).
typedef void (*TiledMapTileCallback)(Texture* tileset_texture, int tileset_width, int tileset_height,
    float dest_x, float dest_y, float dest_w, float dest_h,
    float src_x, float src_y, float src_w, float src_h, void* user_data);

// Iterates every tile of `map`, across all layers, visible within a `viewport_width` x
// `viewport_height` camera window positioned at (camera_x, camera_y) (plus TILED_MAP_CULLING_MARGIN
// tiles of overflow on every side), calling `callback` for each one.
void tiled_map_draw(const TiledMap* map, Texture* tileset_texture, int tileset_width, int tileset_height,
    float camera_x, float camera_y, int viewport_width, int viewport_height,
    TiledMapTileCallback callback, void* user_data);

// Same as tiled_map_draw(), for a single layer only (e.g. an editor toggling layer visibility).
// `tile_scale` scales every tile's on-screen size (1 = native size), with the camera expressed in
// scaled pixels too : what an editor's zoom needs, while culling still matches what's on screen.
void tiled_map_draw_layer(const TiledMap* map, int layer_index, Texture* tileset_texture, int tileset_width, int tileset_height,
    float camera_x, float camera_y, int viewport_width, int viewport_height, float tile_scale,
    TiledMapTileCallback callback, void* user_data);

#ifdef __cplusplus
}
#endif

#endif // _TILED_MAP_RENDER_H_
