// The JSON parser : recursive descent over a NUL-terminated string, every node allocated on the heap
// and freed by json_destroy(). Escapes are decoded, except \uXXXX ones, which are skipped.

#include "json/json.h"

#include <SDL3/SDL.h>

typedef struct
{
    const char* cursor;
} Parser;

static void skip_whitespace(Parser* parser)
{
    while (*parser->cursor == ' ' || *parser->cursor == '\t' || *parser->cursor == '\n' || *parser->cursor == '\r')
    {
        parser->cursor++;
    }
}

static JsonValue* json_alloc(JsonType type)
{
    JsonValue* value = SDL_calloc(1, sizeof(JsonValue));
    value->type = type;
    return value;
}

static JsonValue* parse_value(Parser* parser);

static char* parse_raw_string(Parser* parser)
{
    parser->cursor++; // opening quote

    size_t capacity = 32;
    size_t length = 0;
    char* buffer = SDL_malloc(capacity);

    while (*parser->cursor && *parser->cursor != '"')
    {
        char c = *parser->cursor;

        if (c == '\\')
        {
            parser->cursor++;
            char escaped = *parser->cursor;

            if (escaped == 'u')
            {
                // Tiled's own JSON output never emits \u escapes for the fields this loader
                // reads (paths, names): skip the 4 hex digits rather than decoding UTF-16.
                parser->cursor++;
                for (int i = 0; i < 4 && *parser->cursor; i++)
                {
                    parser->cursor++;
                }
                continue;
            }

            switch (escaped)
            {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default: c = escaped; break;
            }
        }

        if (length + 1 >= capacity)
        {
            capacity *= 2;
            buffer = SDL_realloc(buffer, capacity);
        }

        buffer[length++] = c;
        parser->cursor++;
    }

    if (*parser->cursor == '"')
    {
        parser->cursor++;
    }

    buffer[length] = '\0';

    return buffer;
}

static JsonValue* parse_string_value(Parser* parser)
{
    JsonValue* value = json_alloc(JSON_STRING);
    value->as.string = parse_raw_string(parser);
    return value;
}

static JsonValue* parse_number(Parser* parser)
{
    const char* start = parser->cursor;

    if (*parser->cursor == '-')
    {
        parser->cursor++;
    }

    while (SDL_isdigit((unsigned char)*parser->cursor))
    {
        parser->cursor++;
    }

    if (*parser->cursor == '.')
    {
        parser->cursor++;

        while (SDL_isdigit((unsigned char)*parser->cursor))
        {
            parser->cursor++;
        }
    }

    if (*parser->cursor == 'e' || *parser->cursor == 'E')
    {
        parser->cursor++;

        if (*parser->cursor == '+' || *parser->cursor == '-')
        {
            parser->cursor++;
        }

        while (SDL_isdigit((unsigned char)*parser->cursor))
        {
            parser->cursor++;
        }
    }

    char buffer[64];
    size_t length = (size_t)(parser->cursor - start);

    if (length >= sizeof(buffer))
    {
        length = sizeof(buffer) - 1;
    }

    SDL_memcpy(buffer, start, length);
    buffer[length] = '\0';

    JsonValue* value = json_alloc(JSON_NUMBER);
    value->as.number = SDL_strtod(buffer, NULL);

    return value;
}

static JsonValue* parse_array(Parser* parser)
{
    parser->cursor++; // '['

    JsonValue* value = json_alloc(JSON_ARRAY);
    size_t capacity = 8;
    value->as.array.items = SDL_malloc(sizeof(JsonValue*) * capacity);
    value->as.array.count = 0;

    skip_whitespace(parser);

    if (*parser->cursor == ']')
    {
        parser->cursor++;
        return value;
    }

    while (1)
    {
        skip_whitespace(parser);

        JsonValue* item = parse_value(parser);

        if (!item)
        {
            json_destroy(value);
            return NULL;
        }

        if (value->as.array.count >= capacity)
        {
            capacity *= 2;
            value->as.array.items = SDL_realloc(value->as.array.items, sizeof(JsonValue*) * capacity);
        }

        value->as.array.items[value->as.array.count++] = item;

        skip_whitespace(parser);

        if (*parser->cursor == ',')
        {
            parser->cursor++;
            continue;
        }

        break;
    }

    skip_whitespace(parser);

    if (*parser->cursor == ']')
    {
        parser->cursor++;
    }

    return value;
}

static JsonValue* parse_object(Parser* parser)
{
    parser->cursor++; // '{'

    JsonValue* value = json_alloc(JSON_OBJECT);
    size_t capacity = 8;
    value->as.object.keys = SDL_malloc(sizeof(char*) * capacity);
    value->as.object.values = SDL_malloc(sizeof(JsonValue*) * capacity);
    value->as.object.count = 0;

    skip_whitespace(parser);

    if (*parser->cursor == '}')
    {
        parser->cursor++;
        return value;
    }

    while (1)
    {
        skip_whitespace(parser);

        if (*parser->cursor != '"')
        {
            json_destroy(value);
            return NULL;
        }

        char* key = parse_raw_string(parser);

        skip_whitespace(parser);

        if (*parser->cursor != ':')
        {
            SDL_free(key);
            json_destroy(value);
            return NULL;
        }

        parser->cursor++;

        JsonValue* item = parse_value(parser);

        if (!item)
        {
            SDL_free(key);
            json_destroy(value);
            return NULL;
        }

        if (value->as.object.count >= capacity)
        {
            capacity *= 2;
            value->as.object.keys = SDL_realloc(value->as.object.keys, sizeof(char*) * capacity);
            value->as.object.values = SDL_realloc(value->as.object.values, sizeof(JsonValue*) * capacity);
        }

        value->as.object.keys[value->as.object.count] = key;
        value->as.object.values[value->as.object.count] = item;
        value->as.object.count++;

        skip_whitespace(parser);

        if (*parser->cursor == ',')
        {
            parser->cursor++;
            continue;
        }

        break;
    }

    skip_whitespace(parser);

    if (*parser->cursor == '}')
    {
        parser->cursor++;
    }

    return value;
}

static JsonValue* parse_value(Parser* parser)
{
    skip_whitespace(parser);

    switch (*parser->cursor)
    {
        case '{':
            return parse_object(parser);

        case '[':
            return parse_array(parser);

        case '"':
            return parse_string_value(parser);

        case 't':
            if (SDL_strncmp(parser->cursor, "true", 4) == 0)
            {
                parser->cursor += 4;
                JsonValue* value = json_alloc(JSON_BOOL);
                value->as.boolean = 1;
                return value;
            }
            return NULL;

        case 'f':
            if (SDL_strncmp(parser->cursor, "false", 5) == 0)
            {
                parser->cursor += 5;
                JsonValue* value = json_alloc(JSON_BOOL);
                value->as.boolean = 0;
                return value;
            }
            return NULL;

        case 'n':
            if (SDL_strncmp(parser->cursor, "null", 4) == 0)
            {
                parser->cursor += 4;
                return json_alloc(JSON_NULL);
            }
            return NULL;

        default:
            if (*parser->cursor == '-' || SDL_isdigit((unsigned char)*parser->cursor))
            {
                return parse_number(parser);
            }
            return NULL;
    }
}

JsonValue* json_parse(const char* text)
{
    Parser parser = { text };
    return parse_value(&parser);
}

void json_destroy(JsonValue* value)
{
    if (!value)
    {
        return;
    }

    switch (value->type)
    {
        case JSON_STRING:
            SDL_free(value->as.string);
            break;

        case JSON_ARRAY:
            for (size_t i = 0; i < value->as.array.count; i++)
            {
                json_destroy(value->as.array.items[i]);
            }
            SDL_free(value->as.array.items);
            break;

        case JSON_OBJECT:
            for (size_t i = 0; i < value->as.object.count; i++)
            {
                SDL_free(value->as.object.keys[i]);
                json_destroy(value->as.object.values[i]);
            }
            SDL_free(value->as.object.keys);
            SDL_free(value->as.object.values);
            break;

        default:
            break;
    }

    SDL_free(value);
}

JsonValue* json_object_get(const JsonValue* object, const char* key)
{
    if (!object || object->type != JSON_OBJECT)
    {
        return NULL;
    }

    for (size_t i = 0; i < object->as.object.count; i++)
    {
        if (SDL_strcmp(object->as.object.keys[i], key) == 0)
        {
            return object->as.object.values[i];
        }
    }

    return NULL;
}

JsonValue* json_array_get(const JsonValue* array, size_t index)
{
    if (!array || array->type != JSON_ARRAY || index >= array->as.array.count)
    {
        return NULL;
    }

    return array->as.array.items[index];
}

size_t json_array_count(const JsonValue* array)
{
    if (!array || array->type != JSON_ARRAY)
    {
        return 0;
    }

    return array->as.array.count;
}

double json_number(const JsonValue* value, double fallback)
{
    if (!value || value->type != JSON_NUMBER)
    {
        return fallback;
    }

    return value->as.number;
}

int json_int(const JsonValue* value, int fallback)
{
    return (int)json_number(value, (double)fallback);
}

const char* json_string(const JsonValue* value, const char* fallback)
{
    if (!value || value->type != JSON_STRING)
    {
        return fallback;
    }

    return value->as.string;
}
