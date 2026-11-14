// A minimal JSON reader, written by hand (no third-party dependency) : json_parse()
// builds a tree of JsonValue nodes from a string (json_destroy() frees it), json_object_get() and the
// other accessors walk it, with a fallback when a key is missing or of another type.

#ifndef _JSON_H_
#define _JSON_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

struct JsonValue
{
    JsonType type;

    union
    {
        int boolean;
        double number;
        char* string;

        struct
        {
            JsonValue** items;
            size_t count;
        } array;

        struct
        {
            char** keys;
            JsonValue** values;
            size_t count;
        } object;
    } as;
};

JsonValue* json_parse(const char* text); // NULL on failure
void json_destroy(JsonValue*);

// Accessors: all return NULL/0/the fallback if `value` is NULL, not the expected type, or
// the key/index doesn't exist, so callers don't need to check every intermediate step by hand.
JsonValue* json_object_get(const JsonValue* object, const char* key);
JsonValue* json_array_get(const JsonValue* array, size_t index);
size_t json_array_count(const JsonValue* array);
double json_number(const JsonValue* value, double fallback);
int json_int(const JsonValue* value, int fallback);
const char* json_string(const JsonValue* value, const char* fallback);

#ifdef __cplusplus
}
#endif

#endif // _JSON_H_
