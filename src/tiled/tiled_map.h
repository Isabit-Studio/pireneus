// Maps made with the Tiled editor (JSON format) : tile layers (zlib-compressed base64)
// and a single tileset. Loading and editing only, independent of any rendering : see
// tiled_map_render.h to draw them. tiled_map_save() writes back what the loader keeps.

#ifndef _TILED_MAP_H_
#define _TILED_MAP_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TiledMap TiledMap;

TiledMap* tiled_map_load(const char* file_path); // NULL on failure; file_path is relative to the executable
void tiled_map_destroy(TiledMap*);

int tiled_map_width(const TiledMap*);       // in tiles
int tiled_map_height(const TiledMap*);      // in tiles
int tiled_map_tile_width(const TiledMap*);  // in pixels
int tiled_map_tile_height(const TiledMap*); // in pixels

int tiled_map_layer_count(const TiledMap*);

// Tile GIDs for `layer_index` (row-major, tiled_map_width() * tiled_map_height() entries).
// A GID of 0 means "no tile"; otherwise, subtract 1 to get a zero-based tileset tile index
// (this loader only supports a single tileset per map, so no firstgid lookup is needed).
const uint32_t* tiled_map_layer_tiles(const TiledMap*, int layer_index);
const char* tiled_map_layer_name(const TiledMap*, int layer_index); // "" if out of range

// Sets one tile GID (0 = no tile). Out-of-range layer/coordinates are ignored.
void tiled_map_set_tile(TiledMap*, int layer_index, int x, int y, uint32_t gid);

// Writes the map back as a Tiled JSON map (layers zlib-compressed + base64-encoded, like the files
// this loader reads), openable by Tiled itself. `file_path` is relative to the executable, like
// tiled_map_load(). Only what this loader keeps is written : map size/tile size, every tile layer
// (name, id, tiles) and the single tileset ; anything else the source file had (object layers,
// custom properties, ...) is not preserved. Returns 0 on failure, 1 on success.
int tiled_map_save(const TiledMap*, const char* file_path);

const char* tiled_map_tileset_image(const TiledMap*); // path to the tileset PNG, relative to the map file
int tiled_map_tileset_columns(const TiledMap*);
int tiled_map_tileset_margin(const TiledMap*);
int tiled_map_tileset_spacing(const TiledMap*);
int tiled_map_tileset_tile_count(const TiledMap*);

#ifdef __cplusplus
}
#endif

#endif // _TILED_MAP_H_
