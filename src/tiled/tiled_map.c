// Tiled JSON maps : parsed with json.h, layers decoded from base64 and inflated with miniz.

#include "tiled/tiled_map.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdarg.h>

#include "json/json.h"
#include "miniz.h"
#include "debug.h"

#define TILED_MAX_LAYERS 16

typedef struct
{
    uint32_t* tiles;
    char name[128];
    int id;
} TiledLayer;

struct TiledMap
{
    int width, height;
    int tile_width, tile_height;

    int layer_count;
    int next_layer_id;
    TiledLayer layers[TILED_MAX_LAYERS];
    char background_color[32]; // "" if the map has none

    // Kept from the source file only so tiled_map_save() can write a complete tileset entry back.
    char tileset_name[128];
    char tileset_image[256];
    int tileset_first_gid;
    int tileset_columns;
    int tileset_margin;
    int tileset_spacing;
    int tileset_tile_count;
    int tileset_image_width;
    int tileset_image_height;
};

static int base64_char_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static uint8_t* base64_decode(const char* input, size_t* out_size)
{
    size_t input_length = SDL_strlen(input);
    uint8_t* output = SDL_malloc(input_length / 4 * 3 + 4);
    size_t out_index = 0;

    uint32_t buffer = 0;
    int bits_collected = 0;

    for (size_t i = 0; i < input_length; i++)
    {
        char c = input[i];

        if (c == '=')
        {
            break;
        }

        int value = base64_char_value(c);

        if (value < 0)
        {
            continue; // skip whitespace/newlines, if any
        }

        buffer = (buffer << 6) | (uint32_t)value;
        bits_collected += 6;

        if (bits_collected >= 8)
        {
            bits_collected -= 8;
            output[out_index++] = (uint8_t)((buffer >> bits_collected) & 0xFF);
        }
    }

    *out_size = out_index;

    return output;
}

static uint32_t* decode_layer_data(const char* base64_data, int tile_count)
{
    size_t compressed_size = 0;
    uint8_t* compressed = base64_decode(base64_data, &compressed_size);

    size_t expected_size = (size_t)tile_count * sizeof(uint32_t);
    uint8_t* decompressed = SDL_malloc(expected_size);

    mz_ulong decompressed_size = (mz_ulong)expected_size;
    int result = mz_uncompress(decompressed, &decompressed_size, compressed, (mz_ulong)compressed_size);

    SDL_free(compressed);

    if (result != MZ_OK || decompressed_size != expected_size)
    {
        printf("Could not inflate a Tiled tile layer (miniz error %d).\n", result);
        SDL_free(decompressed);
        return NULL;
    }

    // Tile GIDs are stored little-endian in the Tiled format; every platform this engine
    // currently targets (Windows/macOS/Linux on x86-64/ARM64) is little-endian too, so the
    // decompressed bytes can be reinterpreted directly as a uint32_t array.
    return (uint32_t*)decompressed;
}

TiledMap* tiled_map_load(const char* file_path)
{
    const char* base_path = SDL_GetBasePath();

    char path[512];
    SDL_snprintf(path, sizeof(path), "%s%s", base_path ? base_path : "", file_path);

    size_t text_size = 0;
    char* text = SDL_LoadFile(path, &text_size);

    if (!text)
    {
        printf("Could not read Tiled map '%s': %s\n", path, SDL_GetError());
        return NULL;
    }

    JsonValue* root = json_parse(text);
    SDL_free(text);

    if (!root)
    {
        printf("Could not parse Tiled map '%s'.\n", path);
        return NULL;
    }

    TiledMap* map = SDL_calloc(1, sizeof(TiledMap));
    map->width = json_int(json_object_get(root, "width"), 0);
    map->height = json_int(json_object_get(root, "height"), 0);
    map->tile_width = json_int(json_object_get(root, "tilewidth"), 0);
    map->tile_height = json_int(json_object_get(root, "tileheight"), 0);
    map->next_layer_id = json_int(json_object_get(root, "nextlayerid"), 0);
    SDL_snprintf(map->background_color, sizeof(map->background_color), "%s",
        json_string(json_object_get(root, "backgroundcolor"), ""));

    JsonValue* tilesets = json_object_get(root, "tilesets");
    JsonValue* first_tileset = json_array_get(tilesets, 0);

    if (first_tileset)
    {
        SDL_snprintf(map->tileset_image, sizeof(map->tileset_image), "%s",
            json_string(json_object_get(first_tileset, "image"), ""));
        map->tileset_columns = json_int(json_object_get(first_tileset, "columns"), 0);
        map->tileset_margin = json_int(json_object_get(first_tileset, "margin"), 0);
        map->tileset_spacing = json_int(json_object_get(first_tileset, "spacing"), 0);
        map->tileset_first_gid = json_int(json_object_get(first_tileset, "firstgid"), 1);
        map->tileset_tile_count = json_int(json_object_get(first_tileset, "tilecount"), 0);
        map->tileset_image_width = json_int(json_object_get(first_tileset, "imagewidth"), 0);
        map->tileset_image_height = json_int(json_object_get(first_tileset, "imageheight"), 0);
        SDL_snprintf(map->tileset_name, sizeof(map->tileset_name), "%s",
            json_string(json_object_get(first_tileset, "name"), "tileset"));
    }

    JsonValue* layers = json_object_get(root, "layers");
    size_t layer_count = json_array_count(layers);
    int tile_count = map->width * map->height;

    for (size_t i = 0; i < layer_count && map->layer_count < TILED_MAX_LAYERS; i++)
    {
        JsonValue* layer = json_array_get(layers, i);
        const char* layer_type = json_string(json_object_get(layer, "type"), "");

        if (SDL_strcmp(layer_type, "tilelayer") != 0)
        {
            continue;
        }

        const char* data = json_string(json_object_get(layer, "data"), NULL);

        if (!data)
        {
            continue;
        }

        uint32_t* tiles = decode_layer_data(data, tile_count);

        if (!tiles)
        {
            continue;
        }

        TiledLayer* destination = &map->layers[map->layer_count];
        destination->tiles = tiles;
        destination->id = json_int(json_object_get(layer, "id"), map->layer_count + 1);
        SDL_snprintf(destination->name, sizeof(destination->name), "%s",
            json_string(json_object_get(layer, "name"), "Layer"));
        map->layer_count++;
    }

    json_destroy(root);

    DEBUG_LOG("[tiled] loaded \"%s\": %dx%d tiles, %d layer(s)\n", path, map->width, map->height, map->layer_count);

    return map;
}

void tiled_map_destroy(TiledMap* map)
{
    if (!map)
    {
        return;
    }

    for (int i = 0; i < map->layer_count; i++)
    {
        SDL_free(map->layers[i].tiles);
    }

    SDL_free(map);

    DEBUG_LOG("[tiled] map destroyed\n");
}

int tiled_map_width(const TiledMap* map) { return map->width; }
int tiled_map_height(const TiledMap* map) { return map->height; }
int tiled_map_tile_width(const TiledMap* map) { return map->tile_width; }
int tiled_map_tile_height(const TiledMap* map) { return map->tile_height; }
int tiled_map_layer_count(const TiledMap* map) { return map->layer_count; }

const uint32_t* tiled_map_layer_tiles(const TiledMap* map, int layer_index)
{
    if (layer_index < 0 || layer_index >= map->layer_count)
    {
        return NULL;
    }

    return map->layers[layer_index].tiles;
}

const char* tiled_map_layer_name(const TiledMap* map, int layer_index)
{
    if (layer_index < 0 || layer_index >= map->layer_count)
    {
        return "";
    }

    return map->layers[layer_index].name;
}

void tiled_map_set_tile(TiledMap* map, int layer_index, int x, int y, uint32_t gid)
{
    if (layer_index < 0 || layer_index >= map->layer_count || x < 0 || y < 0 || x >= map->width || y >= map->height)
    {
        return;
    }

    map->layers[layer_index].tiles[y * map->width + x] = gid;
}

// --- Saving ---------------------------------------------------------------------------------

// A growable text buffer, just enough to build the JSON document in memory before one file write.
typedef struct
{
    char* data;
    size_t length;
    size_t capacity;
    int failed; // set once an allocation fails ; every later append is a no-op
} TextBuffer;

static void text_reserve(TextBuffer* buffer, size_t extra)
{
    if (buffer->failed || buffer->length + extra + 1 <= buffer->capacity)
    {
        return;
    }

    size_t capacity = buffer->capacity ? buffer->capacity : 4096;

    while (capacity < buffer->length + extra + 1)
    {
        capacity *= 2;
    }

    char* data = SDL_realloc(buffer->data, capacity);

    if (!data)
    {
        buffer->failed = 1;
        return;
    }

    buffer->data = data;
    buffer->capacity = capacity;
}

static void text_append(TextBuffer* buffer, const char* text, size_t length)
{
    text_reserve(buffer, length);

    if (buffer->failed)
    {
        return;
    }

    SDL_memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
}

static void text_printf(TextBuffer* buffer, SDL_PRINTF_FORMAT_STRING const char* format, ...)
{
    char small[512];

    va_list args;
    va_start(args, format);
    int length = SDL_vsnprintf(small, sizeof(small), format, args);
    va_end(args);

    if (length >= 0 && (size_t)length < sizeof(small))
    {
        text_append(buffer, small, (size_t)length);
    }
}

// A JSON string literal, quotes included. Names in Tiled maps are plain text, so only the characters
// JSON requires escaping are handled (quote, backslash, control characters).
static void text_append_json_string(TextBuffer* buffer, const char* text)
{
    text_append(buffer, "\"", 1);

    for (const char* c = text; *c; c++)
    {
        if (*c == '"' || *c == '\\')
        {
            char escaped[2] = { '\\', *c };
            text_append(buffer, escaped, 2);
        }
        else if ((unsigned char)*c < 0x20)
        {
            text_printf(buffer, "\\u%04x", (unsigned char)*c);
        }
        else
        {
            text_append(buffer, c, 1);
        }
    }

    text_append(buffer, "\"", 1);
}

static void text_append_base64(TextBuffer* buffer, const uint8_t* bytes, size_t size)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    text_reserve(buffer, (size + 2) / 3 * 4);

    for (size_t i = 0; i < size; i += 3)
    {
        uint32_t chunk = (uint32_t)bytes[i] << 16;
        size_t remaining = size - i;

        if (remaining > 1) chunk |= (uint32_t)bytes[i + 1] << 8;
        if (remaining > 2) chunk |= (uint32_t)bytes[i + 2];

        char quad[4] = {
            alphabet[(chunk >> 18) & 63],
            alphabet[(chunk >> 12) & 63],
            remaining > 1 ? alphabet[(chunk >> 6) & 63] : '=',
            remaining > 2 ? alphabet[chunk & 63] : '='
        };

        text_append(buffer, quad, 4);
    }
}

// The inverse of decode_layer_data() : zlib-compress the raw little-endian GIDs, then base64 them.
static int append_layer_data(TextBuffer* buffer, const uint32_t* tiles, int tile_count)
{
    mz_ulong source_size = (mz_ulong)((size_t)tile_count * sizeof(uint32_t));
    mz_ulong compressed_size = mz_compressBound(source_size);
    uint8_t* compressed = SDL_malloc(compressed_size);

    if (!compressed)
    {
        return 0;
    }

    int result = mz_compress(compressed, &compressed_size, (const uint8_t*)tiles, source_size);

    if (result != MZ_OK)
    {
        printf("Could not compress a Tiled tile layer (miniz error %d).\n", result);
        SDL_free(compressed);
        return 0;
    }

    text_append(buffer, "\"", 1);
    text_append_base64(buffer, compressed, compressed_size);
    text_append(buffer, "\"", 1);

    SDL_free(compressed);

    return 1;
}

int tiled_map_save(const TiledMap* map, const char* file_path)
{
    const char* base_path = SDL_GetBasePath();

    char path[512];
    SDL_snprintf(path, sizeof(path), "%s%s", base_path ? base_path : "", file_path);

    TextBuffer buffer = { 0 };
    int next_layer_id = map->next_layer_id;

    text_printf(&buffer, "{\n");

    if (map->background_color[0])
    {
        text_printf(&buffer, " \"backgroundcolor\":");
        text_append_json_string(&buffer, map->background_color);
        text_printf(&buffer, ",\n");
    }

    text_printf(&buffer, " \"compressionlevel\":-1,\n");
    text_printf(&buffer, " \"height\":%d,\n", map->height);
    text_printf(&buffer, " \"infinite\":false,\n");
    text_printf(&buffer, " \"layers\":[\n");

    for (int i = 0; i < map->layer_count; i++)
    {
        const TiledLayer* layer = &map->layers[i];

        if (layer->id >= next_layer_id)
        {
            next_layer_id = layer->id + 1;
        }

        text_printf(&buffer, "  {\n   \"compression\":\"zlib\",\n   \"data\":");

        if (!append_layer_data(&buffer, layer->tiles, map->width * map->height))
        {
            SDL_free(buffer.data);
            return 0;
        }

        text_printf(&buffer, ",\n   \"encoding\":\"base64\",\n");
        text_printf(&buffer, "   \"height\":%d,\n   \"id\":%d,\n   \"name\":", map->height, layer->id);
        text_append_json_string(&buffer, layer->name);
        text_printf(&buffer, ",\n   \"opacity\":1,\n   \"type\":\"tilelayer\",\n   \"visible\":true,\n");
        text_printf(&buffer, "   \"width\":%d,\n   \"x\":0,\n   \"y\":0\n  }%s\n", map->width, i + 1 < map->layer_count ? "," : "");
    }

    text_printf(&buffer, " ],\n");
    text_printf(&buffer, " \"nextlayerid\":%d,\n", next_layer_id);
    text_printf(&buffer, " \"nextobjectid\":1,\n");
    text_printf(&buffer, " \"orientation\":\"orthogonal\",\n");
    text_printf(&buffer, " \"renderorder\":\"right-down\",\n");
    text_printf(&buffer, " \"tileheight\":%d,\n", map->tile_height);
    text_printf(&buffer, " \"tilesets\":[\n  {\n");
    text_printf(&buffer, "   \"columns\":%d,\n   \"firstgid\":%d,\n   \"image\":", map->tileset_columns, map->tileset_first_gid);
    text_append_json_string(&buffer, map->tileset_image);
    text_printf(&buffer, ",\n   \"imageheight\":%d,\n   \"imagewidth\":%d,\n", map->tileset_image_height, map->tileset_image_width);
    text_printf(&buffer, "   \"margin\":%d,\n   \"name\":", map->tileset_margin);
    text_append_json_string(&buffer, map->tileset_name);
    text_printf(&buffer, ",\n   \"spacing\":%d,\n   \"tilecount\":%d,\n", map->tileset_spacing, map->tileset_tile_count);
    text_printf(&buffer, "   \"tileheight\":%d,\n   \"tilewidth\":%d\n  }\n ],\n", map->tile_height, map->tile_width);
    text_printf(&buffer, " \"tilewidth\":%d,\n", map->tile_width);
    text_printf(&buffer, " \"type\":\"map\",\n");
    text_printf(&buffer, " \"version\":\"1.10\",\n");
    text_printf(&buffer, " \"width\":%d\n}\n", map->width);

    if (buffer.failed)
    {
        printf("Out of memory while serializing Tiled map '%s'.\n", path);
        SDL_free(buffer.data);
        return 0;
    }

    int saved = SDL_SaveFile(path, buffer.data, buffer.length);

    if (!saved)
    {
        printf("Could not write Tiled map '%s': %s\n", path, SDL_GetError());
    }
    else
    {
        DEBUG_LOG("[tiled] saved \"%s\" (%zu bytes)\n", path, buffer.length);
    }

    SDL_free(buffer.data);

    return saved ? 1 : 0;
}

const char* tiled_map_tileset_image(const TiledMap* map) { return map->tileset_image; }
int tiled_map_tileset_columns(const TiledMap* map) { return map->tileset_columns; }
int tiled_map_tileset_margin(const TiledMap* map) { return map->tileset_margin; }
int tiled_map_tileset_spacing(const TiledMap* map) { return map->tileset_spacing; }
int tiled_map_tileset_tile_count(const TiledMap* map) { return map->tileset_tile_count; }
