#include "tiled/tiled_map_render.h"

#include <SDL3/SDL.h>

void tiled_map_draw_layer(const TiledMap* map, int layer_index, Texture* tileset_texture, int tileset_width, int tileset_height,
    float camera_x, float camera_y, int viewport_width, int viewport_height, float tile_scale,
    TiledMapTileCallback callback, void* user_data)
{
    const uint32_t* tiles = tiled_map_layer_tiles(map, layer_index);

    if (!tiles || tile_scale <= 0.0f)
    {
        return;
    }

    int tile_width = tiled_map_tile_width(map);
    int tile_height = tiled_map_tile_height(map);
    int map_width = tiled_map_width(map);
    int map_height = tiled_map_height(map);
    int columns = tiled_map_tileset_columns(map);
    int margin = tiled_map_tileset_margin(map);
    int spacing = tiled_map_tileset_spacing(map);

    float dest_tile_width = (float)tile_width * tile_scale;
    float dest_tile_height = (float)tile_height * tile_scale;

    int first_col = (int)SDL_floorf(camera_x / dest_tile_width) - TILED_MAP_CULLING_MARGIN;
    int first_row = (int)SDL_floorf(camera_y / dest_tile_height) - TILED_MAP_CULLING_MARGIN;
    int visible_cols = (int)((float)viewport_width / dest_tile_width) + 1 + 2 * TILED_MAP_CULLING_MARGIN;
    int visible_rows = (int)((float)viewport_height / dest_tile_height) + 1 + 2 * TILED_MAP_CULLING_MARGIN;

    int last_col = SDL_min(map_width, first_col + visible_cols);
    int last_row = SDL_min(map_height, first_row + visible_rows);
    first_col = SDL_max(0, first_col);
    first_row = SDL_max(0, first_row);

    for (int row = first_row; row < last_row; row++)
    {
        for (int col = first_col; col < last_col; col++)
        {
            uint32_t gid = tiles[row * map_width + col];

            if (gid == 0)
            {
                continue;
            }

            int tile_index = (int)gid - 1;
            int tile_col = tile_index % columns;
            int tile_row = tile_index / columns;

            float src_x = (float)(margin + tile_col * (tile_width + spacing));
            float src_y = (float)(margin + tile_row * (tile_height + spacing));

            float dest_x = (float)col * dest_tile_width - camera_x;
            float dest_y = (float)row * dest_tile_height - camera_y;

            callback(tileset_texture, tileset_width, tileset_height,
                dest_x, dest_y, dest_tile_width, dest_tile_height,
                src_x, src_y, (float)tile_width, (float)tile_height, user_data);
        }
    }
}

void tiled_map_draw(const TiledMap* map, Texture* tileset_texture, int tileset_width, int tileset_height,
    float camera_x, float camera_y, int viewport_width, int viewport_height,
    TiledMapTileCallback callback, void* user_data)
{
    for (int layer = 0; layer < tiled_map_layer_count(map); layer++)
    {
        tiled_map_draw_layer(map, layer, tileset_texture, tileset_width, tileset_height,
            camera_x, camera_y, viewport_width, viewport_height, 1.0f, callback, user_data);
    }
}
